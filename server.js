// =====================================================
// KuetTrack Backend — Node.js + Express + MongoDB Atlas
// =====================================================

const express = require('express');
const mongoose = require('mongoose');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const cors = require('cors');
require('dotenv').config();

// ─── Stripe (optional — server starts fine without it) ─
let stripe = null;
function getStripe() {
  if (stripe) return stripe;
  try {
    const key = process.env.STRIPE_SECRET_KEY;
    const isPlaceholder = !key || key.startsWith('sk_test_xxx') || key.includes('your_actual') || key.includes('your_stripe') || key.length < 32;
    if (isPlaceholder) {
      console.warn('⚠️  STRIPE_SECRET_KEY missing or placeholder in .env');
      return null;
    }
    stripe = require('stripe')(key);
    console.log('✅ Stripe initialised');
    return stripe;
  } catch (e) {
    console.warn('⚠️  stripe package not installed. Run: npm install stripe');
    return null;
  }
}
// Attempt init at startup (non-fatal)
getStripe();

const app = express();

// ─── Middleware ───────────────────────────────────────
app.use(express.json());
app.use(cors({
  origin: '*',
  methods: ['GET', 'POST', 'PUT', 'PATCH', 'DELETE'],
  allowedHeaders: ['Content-Type', 'Authorization']
}));

// ─── Real-time RFID State ─────────────────────────────
let latestRfidDetection = null;
let rfidDetectionQueue = [];

// ─── Real-time GPS State (ESP32 #2 — no WiFi on device, uses SIM800L or direct WiFi) ─
// ESP32 #2 POSTs to /api/gps/update  →  website polls /api/gps/latest
let latestGps = null;          // { lat, lon, speed, satellites, deviceId, timestamp }
let gpsHistory = [];           // rolling last 300 points for trail

// ─── Bike Pending Commands ─────────────────────────────────────────────
// When the user clicks "Start Ride" on the dashboard, a pending unlock
// command is stored here.  The ESP32 polls GET /api/bikes/:bikeId/command
// to retrieve and clear it, then unlocks the solenoid.
// Format: { 'BIKE-001': { command: 'unlock', rfidUid: '...', rideId: '...' } }
const bikeCommands = {};

// ─── Bike Pending Lock Commands (set when website ends a ride) ─────────────
// ESP32 picks these up via the same GET /api/bikes/:bikeId/command endpoint
const bikeLockCommands = {};


// ─── MQTT Real-Time Layer ─────────────────────────────────────────────────────
// Using HiveMQ public broker (no account needed). For production swap to
// HiveMQ Cloud / EMQX Cloud free tier and set MQTT_BROKER in .env.
//
// Topics:
//   kuettrack/BIKE-001/gps   ← ESP32 publishes GPS  → server saves to DB + forwards
//   kuettrack/BIKE-001/rfid  ← ESP32 publishes RFID scan → server auth → publishes auth result
//   kuettrack/BIKE-001/auth  → server publishes auth result → ESP32 reads
//   kuettrack/BIKE-001/cmd   → server publishes unlock/lock → ESP32 reads instantly
//   kuettrack/BIKE-001/status← ESP32 LWT (online/offline)
// ─────────────────────────────────────────────────────────────────────────────
let mqttClient = null;

function initMqtt() {
  try {
    const mqtt = require('mqtt');
    const broker   = process.env.MQTT_BROKER   || 'mqtt://broker.hivemq.com';
    const mqttUser = process.env.MQTT_USERNAME  || '';
    const mqttPass = process.env.MQTT_PASSWORD  || '';
    const opts = {
      clientId: 'kuettrack-server-' + Math.random().toString(16).slice(2, 8),
      clean: true,
      reconnectPeriod: 3000,
      connectTimeout: 10000,
    };
    if (mqttUser) { opts.username = mqttUser; opts.password = mqttPass; }

    mqttClient = mqtt.connect(broker, opts);

    mqttClient.on('connect', () => {
      console.log('✅ MQTT connected to ' + broker);
      mqttClient.subscribe('kuettrack/+/gps',   { qos: 1 });
      mqttClient.subscribe('kuettrack/+/rfid',  { qos: 1 });
      mqttClient.subscribe('kuettrack/+/status',{ qos: 0 });
    });

    mqttClient.on('error',   e => console.warn('[MQTT] error:', e.message));
    mqttClient.on('offline', () => console.warn('[MQTT] offline'));
    mqttClient.on('reconnect', () => console.log('[MQTT] reconnecting...'));

    mqttClient.on('message', async (topic, payload) => {
      try {
        const parts   = topic.split('/');   // ['kuettrack','BIKE-001','gps']
        const bikeId  = parts[1];
        const msgType = parts[2];
        const data    = JSON.parse(payload.toString());

        // ── GPS message from ESP32 ──────────────────────────────────────────
        if (msgType === 'gps') {
          const { lat, lon, speed, altitude, satellites, hasFix, deviceId } = data;
          const timestamp = Date.now();
          const fix = hasFix !== false;
          const point = { lat: parseFloat(lat)||0, lon: parseFloat(lon)||0,
            speed: parseFloat(speed)||0, altitude: parseFloat(altitude)||0,
            satellites: parseInt(satellites)||0, deviceId, timestamp, hasFix: fix };

          if (fix) {
            latestGps = point;
            gpsHistory.push(point);
            if (gpsHistory.length > 300) gpsHistory.shift();
          } else if (latestGps) {
            latestGps.timestamp  = timestamp;
            latestGps.satellites = point.satellites;
            latestGps.hasFix     = false;
          }

          // Save to DB (non-blocking)
          try {
            const user = await User.findOne({ rfidUid: deviceId });
            if (user && fix) {
              await GpsLocation.create({
                userId: user._id, deviceId,
                latitude: point.lat, longitude: point.lon,
                speed: point.speed, altitude: point.altitude,
                satellites: point.satellites,
                timestamp: new Date(timestamp), isLive: true
              });
            }
          } catch (dbErr) { console.warn('[MQTT GPS] DB save failed:', dbErr.message); }
          console.log(`📍 [MQTT GPS] ${bikeId} lat=${point.lat} lon=${point.lon} fix=${fix}`);
        }

        // ── RFID scan from ESP32 ────────────────────────────────────────────
        if (msgType === 'rfid') {
          const { uid, stationId } = data;
          if (!uid) return;
          const rfidUid = uid.toUpperCase().trim();
          const user    = await User.findOne({ rfidUid: new RegExp('^' + rfidUid + '$', 'i'), isActive: true }).select('-password');

          if (!user) {
            mqttPublish(`kuettrack/${bikeId}/auth`, { uid: rfidUid, authorized: false, message: 'Card not registered' });
            console.log(`❌ [MQTT RFID] denied: ${rfidUid}`);
            return;
          }

          const activeRide = await Ride.findOne({ userId: user._id, status: 'active' });
          let lockAction;
          if (activeRide) {
            lockAction = 'lock';
            const endTime    = new Date();
            const elapsedSec = Math.floor((endTime - activeRide.startTime) / 1000);
            const mins = Math.floor(elapsedSec / 60), secs = elapsedSec % 60;
            const duration   = `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}`;
            const distanceKm = parseFloat((elapsedSec * 0.00278).toFixed(2));
            const fare       = Math.max(Math.ceil(elapsedSec / 60) * 2, 2);
            const newBalance = Math.max((user.walletBalance || 0) - fare, 0);
            await User.findByIdAndUpdate(user._id, { walletBalance: newBalance });
            await Ride.findByIdAndUpdate(activeRide._id, {
              status: 'completed', endTime, stationEnd: stationId || bikeId,
              duration, distanceKm, fare
            });
            mqttPublish(`kuettrack/${bikeId}/auth`, {
              uid: rfidUid, authorized: true, lockAction: 'lock',
              userName: user.firstName, duration, fare, newBalance
            });
            console.log(`🔒 [MQTT RFID] ride ended for ${rfidUid} fare=৳${fare}`);
          } else {
            lockAction = 'unlock';
            const ride = await Ride.create({ userId: user._id, rfidUid, bikeId, stationStart: stationId || bikeId });
            mqttPublish(`kuettrack/${bikeId}/auth`, {
              uid: rfidUid, authorized: true, lockAction: 'unlock',
              userName: user.firstName, rideId: ride._id.toString()
            });
            console.log(`🔓 [MQTT RFID] ride started for ${rfidUid} → ${user.firstName}`);
          }

          // Also store for website polling (login_rfid.html)
          const token = jwt.sign({ userId: user._id, email: user.email, role: user.role }, process.env.JWT_SECRET, { expiresIn: '7d' });
          latestRfidDetection = {
            uid: rfidUid, authorized: true, timestamp: Date.now(),
            userName: user.firstName + ' ' + user.lastName, token,
            user: { id: user._id, firstName: user.firstName, lastName: user.lastName,
                    email: user.email, plan: user.plan, walletBalance: user.walletBalance, role: user.role }
          };
        }

        // ── Status message from ESP32 ───────────────────────────────────────
        if (msgType === 'status') {
          console.log(`[MQTT STATUS] ${bikeId}: ${JSON.stringify(data)}`);
        }
      } catch (e) {
        console.warn('[MQTT message] parse error:', e.message);
      }
    });
  } catch (e) {
    console.warn('[MQTT] not available — run: npm install mqtt  Error:', e.message);
  }
}

// Helper: publish JSON to a topic (QoS 1, retained=false)
function mqttPublish(topic, payload, retained = false) {
  if (!mqttClient || !mqttClient.connected) return;
  mqttClient.publish(topic, JSON.stringify(payload), { qos: 1, retain: retained });
}

// ─── MongoDB Atlas Connection ─────────────────────────
// ─── MongoDB Atlas Connection ─────────────────────────
const MONGODB_URI = process.env.MONGODB_URI;

mongoose.connect(MONGODB_URI)
.then(() => {
  console.log('✅ Connected to MongoDB Atlas (Cluster0)');
  initMqtt();   // Start MQTT after DB is ready
})
.catch((err) => {
  console.error('❌ MongoDB connection failed:', err.message);
  process.exit(1); // Stop server if DB fails
});

// Optional: Log connection events
mongoose.connection.on('disconnected', () => {
  console.warn('⚠️  MongoDB disconnected. Attempting to reconnect...');
});
mongoose.connection.on('reconnected', () => {
  console.log('🔄 MongoDB reconnected.');
});

// ─── Auto-seed default admin on first run ─────────────
async function seedDefaultAdmin() {
  try {
    const adminEmail = process.env.ADMIN_EMAIL || 'admin@velotrack.com';
    const adminPassword = process.env.ADMIN_PASSWORD;
    if (!adminPassword) {
      console.warn('⚠️  ADMIN_PASSWORD not set in .env. Using secure random password.');
      const crypto = require('crypto');
      process.env.ADMIN_PASSWORD = crypto.randomBytes(12).toString('hex');
    }

    const existing = await User.findOne({ email: adminEmail });
    if (!existing) {
      const hashed = await bcrypt.hash(adminPassword, 12);
      await User.create({
        firstName: 'Super',
        lastName: 'Admin',
        email: adminEmail,
        phone: '0000000000',
        city: 'HQ',
        password: hashed,
        plan: 'basic',
        role: 'admin',
        isActive: true
      });
      console.log(`✅ Default admin created → ${adminEmail}`);
      console.log('   ⚠️  ADMIN_PASSWORD was set in .env. Change it after first login!');
    } else if (existing.role !== 'admin') {
      // Promote to admin if the account exists but isn't admin yet
      await User.findByIdAndUpdate(existing._id, { role: 'admin' });
      console.log(`🔄 Existing account ${adminEmail} promoted to admin.`);
    } else {
      console.log(`ℹ️  Admin account already exists: ${adminEmail}`);
    }

    // ─── Auto-seed demo RFID user ──────────────────────────────
    // Demo user only created if DEMO_PASSWORD is explicitly set in .env
    const demoPassword = process.env.DEMO_PASSWORD;
    const rfidUser = await User.findOne({ rfidUid: new RegExp('^C33B51FE$', 'i') });
    if (!rfidUser && demoPassword) {
      const demoHash = await bcrypt.hash(demoPassword, 12);
      await User.create({
        firstName: 'Wazeeh',
        lastName: 'Tahmin Showmo',
        email: 'wazeeh.tahmin@velotrack.com',
        phone: '01700000001',
        city: 'Dhaka',
        password: demoHash,
        rfidUid: 'C33B51FE',
        plan: 'monthly',
        walletBalance: 500,
        role: 'user',
        isActive: true
      });
      console.log(`✅ Demo RFID user created → Wazeeh Tahmin Showmo (RFID: C33B51FE)`);
    } else {
      console.log(`ℹ️  RFID user already exists: ${rfidUser.email}`);
    }
  } catch (err) {
    console.error('❌ Admin seed error:', err.message);
  }
}

mongoose.connection.once('open', seedDefaultAdmin);

// ─── User Schema ──────────────────────────────────────
const userSchema = new mongoose.Schema({
  firstName:  { type: String, required: true, trim: true },
  lastName:   { type: String, required: true, trim: true },
  email:      { type: String, required: true, unique: true, lowercase: true, trim: true },
  phone:      { type: String, required: true, trim: true },
  city:       { type: String, required: true },
  password:   { type: String, required: true },
  plan:       { type: String, enum: ['basic', 'monthly', 'student'], default: 'basic' },
  idNumber:   { type: String, default: '' },     // NID or Student ID
  rfidUid:    { type: String, default: null },   // Assigned by admin/kiosk later
  walletBalance: { type: Number, default: 0 },
  isActive:   { type: Boolean, default: true },
  role:       { type: String, enum: ['user', 'admin'], default: 'user' },
  creditedSessions: { type: [String], default: [] },
  createdAt:  { type: Date, default: Date.now }
});

const User = mongoose.model('User', userSchema);

// ─── GPS Location Schema ──────────────────────────────
const gpsLocationSchema = new mongoose.Schema({
  userId:     { type: mongoose.Schema.Types.ObjectId, ref: 'User', required: true, index: true },
  deviceId:   { type: String, default: 'ESP32' },
  latitude:   { type: Number, required: true },
  longitude:  { type: Number, required: true },
  speed:      { type: Number, default: 0 },
  altitude:   { type: Number, default: 0 },
  satellites: { type: Number, default: 0 },
  accuracy:   { type: Number, default: 0 },
  timestamp:  { type: Date, default: Date.now },
  isLive:     { type: Boolean, default: true }  // Mark as live tracking session
});

// Create a TTL index to auto-delete records after 30 days
gpsLocationSchema.index({ timestamp: 1 }, { expireAfterSeconds: 2592000 });

const GpsLocation = mongoose.model('GpsLocation', gpsLocationSchema);

// ─── RFID Login Schema ────────────────────────────────
const rfidLoginSchema = new mongoose.Schema({
  userId:      { type: mongoose.Schema.Types.ObjectId, ref: 'User', default: null },
  rfidUid:     { type: String, required: true },
  status:      { type: String, enum: ['success', 'invalid_card', 'error'], required: true },
  stationId:   { type: String, default: 'STATION-001' },
  ipAddress:   { type: String, default: '' },
  timestamp:   { type: Date, default: Date.now },
  deviceInfo:  { type: String, default: 'ESP32' }
});
const RfidLogin = mongoose.model('RfidLogin', rfidLoginSchema);

// ─── Auth Middleware ──────────────────────────────────
const authMiddleware = (req, res, next) => {
  const token = req.headers.authorization?.split(' ')[1];
  if (!token) return res.status(401).json({ message: 'No token provided' });
  try {
    const decoded = jwt.verify(token, process.env.JWT_SECRET);
    req.user = decoded;
    next();
  } catch {
    res.status(401).json({ message: 'Invalid or expired token' });
  }
};

// ─── Routes ───────────────────────────────────────────

// POST /api/auth/register
app.post('/api/auth/register', async (req, res) => {
  try {
    const { firstName, lastName, email, phone, city, password, plan, idNumber } = req.body;

    // Validate required fields
    if (!firstName || !lastName || !email || !phone || !city || !password) {
      return res.status(400).json({ message: 'All required fields must be filled.' });
    }
    if (password.length < 6) {
      return res.status(400).json({ message: 'Password must be at least 6 characters.' });
    }

    // Check if email already exists
    const existing = await User.findOne({ email });
    if (existing) {
      return res.status(409).json({ message: 'An account with this email already exists.' });
    }

    // Hash password
    const hashedPassword = await bcrypt.hash(password, 12);

    // Create user
    const user = new User({
      firstName, lastName, email, phone, city,
      password: hashedPassword,
      plan: plan || 'basic',
      idNumber: idNumber || ''
    });

    await user.save();

    // Generate JWT token
    const token = jwt.sign(
      { userId: user._id, email: user.email, role: user.role },
      process.env.JWT_SECRET,
      { expiresIn: '7d' }
    );

    res.status(201).json({
      message: 'Account created successfully!',
      token,
      user: {
        id: user._id,
        firstName: user.firstName,
        lastName: user.lastName,
        email: user.email,
        phone: user.phone,
        city: user.city,
        plan: user.plan,
        walletBalance: user.walletBalance,
        rfidUid: user.rfidUid
      }
    });
  } catch (err) {
    console.error('Register error:', err);
    res.status(500).json({ message: 'Server error. Please try again.' });
  }
});

// POST /api/auth/login
app.post('/api/auth/login', async (req, res) => {
  try {
    const { email, password } = req.body;

    if (!email || !password) {
      return res.status(400).json({ message: 'Email and password are required.' });
    }

    // Find user
    const user = await User.findOne({ email });
    if (!user) {
      return res.status(401).json({ message: 'Invalid email or password.' });
    }

    // Check account active
    if (!user.isActive) {
      return res.status(403).json({ message: 'Your account has been suspended. Please contact support.' });
    }

    // Compare password
    const isMatch = await bcrypt.compare(password, user.password);
    if (!isMatch) {
      return res.status(401).json({ message: 'Invalid email or password.' });
    }

    // Generate JWT
    const token = jwt.sign(
      { userId: user._id, email: user.email, role: user.role },
      process.env.JWT_SECRET,
      { expiresIn: '7d' }
    );

    res.json({
      message: 'Login successful!',
      token,
      user: {
        id: user._id,
        firstName: user.firstName,
        lastName: user.lastName,
        email: user.email,
        phone: user.phone,
        city: user.city,
        plan: user.plan,
        walletBalance: user.walletBalance,
        rfidUid: user.rfidUid,
        role: user.role
      }
    });
  } catch (err) {
    console.error('Login error:', err);
    res.status(500).json({ message: 'Server error. Please try again.' });
  }
});

// GET /api/auth/me — Get current user profile (protected)
app.get('/api/auth/me', authMiddleware, async (req, res) => {
  try {
    const user = await User.findById(req.user.userId).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });
    res.json(user);
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// PATCH /api/auth/rfid — Admin assigns RFID card to user (protected)
app.patch('/api/auth/rfid', authMiddleware, async (req, res) => {
  try {
    const { userId, rfidUid } = req.body;
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    // Check RFID not already used (case-insensitive)
    const existing = await User.findOne({ rfidUid: new RegExp('^' + rfidUid + '$', 'i') });
    if (existing) {
      return res.status(409).json({ message: 'This RFID card is already assigned to another user.' });
    }
    const user = await User.findByIdAndUpdate(userId, { rfidUid: rfidUid.toUpperCase() }, { new: true }).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });
    res.json({ message: 'RFID card assigned successfully.', user });
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// POST /api/auth/rfid-user — Create or update user with RFID (admin only)
app.post('/api/auth/rfid-user', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    
    const { firstName, lastName, email, phone, city, rfidUid, plan } = req.body;
    
    if (!firstName || !lastName || !email || !phone || !city || !rfidUid) {
      return res.status(400).json({ message: 'All fields (firstName, lastName, email, phone, city, rfidUid) are required.' });
    }
    
    // Check if RFID is already used (case-insensitive)
    const existingByRfid = await User.findOne({ rfidUid: new RegExp('^' + rfidUid + '$', 'i') });
    if (existingByRfid) {
      return res.status(409).json({ message: 'This RFID card is already assigned to another user.' });
    }
    
    // Check if user already exists by email
    let user = await User.findOne({ email });
    if (user) {
      // Update existing user with RFID
      user = await User.findByIdAndUpdate(user._id, { rfidUid }, { new: true }).select('-password');
      return res.json({ message: 'RFID card assigned to existing user.', user });
    }
    
    // Create new user with RFID
    const defaultPassword = 'RfidUser@123'; // Temporary password
    const hashed = await bcrypt.hash(defaultPassword, 12);
    
    user = await User.create({
      firstName,
      lastName,
      email,
      phone,
      city,
      rfidUid: rfidUid.toUpperCase(),
      password: hashed,
      plan: plan || 'basic',
      isActive: true,
      role: 'user'
    });
    
    res.status(201).json({ 
      message: 'User created and RFID card assigned successfully',
      user: { ...user.toObject(), password: undefined },
      tempPassword: defaultPassword
    });
  } catch (err) {
    console.error('Create RFID user error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/auth/user-by-rfid/:uid — ESP32/Arduino calls this to verify RFID tap
app.get('/api/auth/user-by-rfid/:uid', async (req, res) => {
  try {
    const rfidUid = req.params.uid.toUpperCase();
    
    const user = await User.findOne({ rfidUid: new RegExp('^' + rfidUid + '$', 'i'), isActive: true }).select('-password');
    if (!user) {
      // Log failed RFID login
      await RfidLogin.create({
        rfidUid: rfidUid,
        status: 'invalid_card',
        ipAddress: req.ip,
        deviceInfo: req.headers['user-agent'] || 'Unknown'
      });
      return res.status(404).json({ authorized: false, message: 'RFID card not recognized.' });
    }
    
    // Log successful RFID login
    await RfidLogin.create({
      userId: user._id,
      rfidUid: rfidUid,
      status: 'success',
      ipAddress: req.ip,
      deviceInfo: req.headers['user-agent'] || 'Unknown'
    });

    // Generate JWT token (same format as Email login)
    const token = jwt.sign(
      { userId: user._id, email: user.email, role: user.role },
      process.env.JWT_SECRET,
      { expiresIn: '7d' }
    );

    // ✅ Store RFID detection AFTER token is ready — includes all fields the website needs
    latestRfidDetection = {
      uid: rfidUid,
      authorized: true,
      timestamp: Date.now(),
      token,
      userName: user.firstName + ' ' + user.lastName,
      user: {
        id: user._id,
        firstName: user.firstName,
        lastName: user.lastName,
        email: user.email,
        plan: user.plan,
        walletBalance: user.walletBalance,
        role: user.role
      }
    };

    res.json({
      authorized: true,
      token,
      user: {
        id: user._id,
        firstName: user.firstName,
        lastName: user.lastName,
        email: user.email,
        plan: user.plan,
        walletBalance: user.walletBalance,
        role: user.role
      }
    });
  } catch (err) {
    // Log error
    await RfidLogin.create({
      rfidUid: req.params.uid,
      status: 'error',
      ipAddress: req.ip,
      deviceInfo: 'Error'
    }).catch(e => console.error('Failed to log RFID error:', e));
    
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/rfid/latest — Frontend polls this to get latest detected RFID (no auth)
app.get('/api/rfid/latest', (req, res) => {
  if (latestRfidDetection) {
    const response = latestRfidDetection;
    latestRfidDetection = null; // Clear after sending
    return res.json(response);
  }
  res.status(204).send(); // No content
});

// POST /api/rfid/scan — ESP32 #1 (WiFi) pushes scanned UID here directly
// ESP32 #1 reads RFID card → POST { uid, stationId } → server validates → stores result
// Website polls /api/rfid/latest to pick it up and log user in
app.post('/api/rfid/scan', async (req, res) => {
  try {
    const { uid, stationId } = req.body;
    if (!uid) return res.status(400).json({ message: 'uid is required.' });

    const rfidUid = uid.toUpperCase().trim();
    const user = await User.findOne({ rfidUid: new RegExp('^' + rfidUid + '$', 'i'), isActive: true }).select('-password');

    if (!user) {
      // Log failed attempt
      await RfidLogin.create({
        rfidUid,
        status: 'invalid_card',
        stationId: stationId || 'ESP32-STATION',
        ipAddress: req.ip,
        deviceInfo: req.headers['user-agent'] || 'ESP32-WiFi'
      });
      // Still store it so website can show "denied" feedback
      latestRfidDetection = { uid: rfidUid, authorized: false, timestamp: Date.now() };
      console.log(`❌ RFID denied: ${rfidUid}`);
      return res.status(404).json({ authorized: false, message: 'RFID card not recognized.' });
    }

    // Authorized — log and store for website polling
    await RfidLogin.create({
      userId: user._id,
      rfidUid,
      status: 'success',
      stationId: stationId || 'ESP32-STATION',
      ipAddress: req.ip,
      deviceInfo: req.headers['user-agent'] || 'ESP32-WiFi'
    });

    const token = jwt.sign(
      { userId: user._id, email: user.email, role: user.role },
      process.env.JWT_SECRET,
      { expiresIn: '7d' }
    );

    latestRfidDetection = {
      uid: rfidUid,
      authorized: true,
      timestamp: Date.now(),
      userName: user.firstName + ' ' + user.lastName,
      token,
      user: {
        id: user._id,
        firstName: user.firstName,
        lastName: user.lastName,
        email: user.email,
        plan: user.plan,
        walletBalance: user.walletBalance,
        role: user.role
      }
    };

    // ─── 3-State Lock/Unlock Logic ────────────────────────────
    // State A: active ride with pendingCommand:'unlock'
    //          → dashboard selected a bike, user taps RFID to physically unlock
    // State B: active ride with no pendingCommand (ride in progress)
    //          → user taps RFID to end the ride and lock
    // State C: no active ride at all
    //          → just authenticate (ESP32 → STATE_RFID_VERIFIED, shows "Select bike in app")
    const activeRide = await Ride.findOne({ userId: user._id, status: 'active' });

    let lockAction = null;
    let duration, fare;

    if (activeRide && activeRide.pendingCommand === 'unlock') {
      // State A — user selected bike on dashboard, now tapping to unlock solenoid
      lockAction = 'unlock';
      await Ride.findByIdAndUpdate(activeRide._id, { pendingCommand: null });
      console.log(`🔓 RFID tap unlocked bike: ${rfidUid} → ${user.firstName} | ride=${activeRide._id}`);

    } else if (activeRide && !activeRide.pendingCommand) {
      // State B — ride in progress, tap to end ride and lock solenoid
      lockAction = 'lock';
      const endTime    = new Date();
      const elapsedSec = Math.floor((endTime - activeRide.startTime) / 1000);
      const mins = Math.floor(elapsedSec / 60);
      const secs = elapsedSec % 60;
      duration = `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}`;
      const distanceKm = parseFloat((elapsedSec * 0.00278).toFixed(2));
      fare = Math.max(Math.ceil(elapsedSec / 60) * 2, 2);
      const newBalance = Math.max((user.walletBalance || 0) - fare, 0);

      await User.findByIdAndUpdate(user._id, { walletBalance: newBalance });
      await Ride.findByIdAndUpdate(activeRide._id, {
        status: 'completed',
        endTime,
        stationEnd: stationId || activeRide.bikeId || 'BIKE-001',
        duration,
        distanceKm,
        fare
      });
      // Publish MQTT lock immediately
      mqttPublish(`kuettrack/${activeRide.bikeId || 'BIKE-001'}/cmd`, {
        command: 'lock', rideId: activeRide._id.toString()
      });
      console.log(`🔒 Ride ended: ${rfidUid} | duration=${duration} fare=৳${fare} newBalance=৳${newBalance}`);

    }
    // State C — no active ride, verify only (lockAction stays null)

    console.log(`✅ RFID authorized: ${rfidUid} → ${user.firstName} | action: ${lockAction || 'verify-only'}`);
    res.json({
      authorized: true,
      ...(lockAction && { lockAction }),
      message: lockAction === 'unlock'
        ? `Bike unlocked. Enjoy your ride, ${user.firstName}!`
        : lockAction === 'lock'
          ? 'Bike locked. Ride ended.'
          : 'Authenticated. Select a bike in the app, then tap again to unlock.',
      userName: user.firstName,
      ...(lockAction === 'lock' && { duration, fare })
    });
  } catch (err) {
    console.error('RFID scan error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// ─── GPS Routes — ESP32 (GPS + WiFi) ──────────────────
// ESP32 reads GPS via NEO-6M and posts coordinates here via WiFi HTTP
// The website then polls endpoints to show live location on map

// POST /api/gps/update — ESP32 sends GPS coordinates
// Body: { lat, lon, speed, satellites, altitude, deviceId (RFID UID) }
app.post('/api/gps/update', async (req, res) => {
  try {
    const { lat, lon, speed, satellites, altitude, deviceId } = req.body;
    
    if (lat === undefined || lon === undefined) {
      return res.status(400).json({ message: 'lat and lon required.' });
    }
    
    if (!deviceId) {
      return res.status(400).json({ message: 'deviceId (RFID UID) required.' });
    }
    
    const parsed = { 
      lat: parseFloat(lat), 
      lon: parseFloat(lon),
      speed: parseFloat(speed) || 0,
      altitude: parseFloat(altitude) || 0,
      satellites: parseInt(satellites) || 0
    };
    
    if (isNaN(parsed.lat) || isNaN(parsed.lon)) {
      return res.status(400).json({ message: 'lat and lon must be valid numbers.' });
    }

    const timestamp = Date.now();
    const hasFix = req.body.hasFix !== false;  // undefined = legacy ESP32 (assume fix)
    const point = {
      lat: parsed.lat,
      lon: parsed.lon,
      speed: parsed.speed,
      satellites: parsed.satellites,
      altitude: parsed.altitude,
      deviceId: deviceId,
      timestamp: timestamp,
      hasFix: hasFix
    };

    // Update in-memory tracking (always, so status endpoint shows tracker is alive)
    if (hasFix) {
      latestGps = point;
      gpsHistory.push(point);
      if (gpsHistory.length > 300) gpsHistory.shift();
    } else {
      // No fix — update timestamp so /api/gps/status shows tracker is online
      // but don't overwrite the last known valid coords
      if (latestGps) {
        latestGps.timestamp = timestamp;
        latestGps.satellites = parsed.satellites;
        latestGps.hasFix = false;
      }
    }

    // Only store in DB if we have a real fix (hasFix:false = keepalive only)
    // Find user by RFID UID and store location in database
    try {
      const user = await User.findOne({ rfidUid: deviceId });

      if (user && hasFix) {
        // Save GPS location to database
        const gpsRecord = new GpsLocation({
          userId: user._id,
          deviceId: deviceId,
          latitude: parsed.lat,
          longitude: parsed.lon,
          speed: parsed.speed,
          altitude: parsed.altitude,
          satellites: parsed.satellites,
          timestamp: new Date(timestamp),
          isLive: true
        });
        await gpsRecord.save();
        console.log(`📍 GPS saved for user: ${user.firstName} ${user.lastName} (RFID: ${deviceId})`);
      } else {
        console.warn(`⚠️  GPS received but RFID UID not found: ${deviceId}`);
      }
    } catch (dbErr) {
      console.warn('⚠️  Could not save GPS to database:', dbErr.message);
      // Continue anyway - we still have in-memory tracking
    }

    console.log(`📡 GPS: lat=${point.lat.toFixed(5)} lon=${point.lon.toFixed(5)} speed=${point.speed} sats=${point.satellites} RFID=${deviceId}`);
    mqttPublish('kuettrack/BIKE-001/gps', point);
    res.json({ received: true, timestamp: timestamp, deviceId: deviceId });
  } catch (err) {
    console.error('GPS update error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/gps/latest — Get latest GPS location (no auth)
app.get('/api/gps/latest', (req, res) => {
  if (!latestGps) return res.status(204).send();
  res.json(latestGps);
});

// GET /api/gps/history — Get GPS trail (last 300 points)
app.get('/api/gps/history', (req, res) => {
  res.json(gpsHistory);
});

// GET /api/gps/user/:userId — Get user's latest GPS location (protected)
app.get('/api/gps/user/:userId', authMiddleware, async (req, res) => {
  try {
    const { userId } = req.params;
    
    // Users can only see their own location, admins can see any
    if (req.user.role !== 'admin' && req.user.userId !== userId) {
      return res.status(403).json({ message: 'Access denied.' });
    }

    const latestLocation = await GpsLocation.findOne({ userId })
      .sort({ timestamp: -1 })
      .limit(1);

    if (!latestLocation) {
      return res.status(404).json({ message: 'No GPS data found for this user.' });
    }

    res.json({
      userId: latestLocation.userId,
      latitude: latestLocation.latitude,
      longitude: latestLocation.longitude,
      speed: latestLocation.speed,
      altitude: latestLocation.altitude,
      satellites: latestLocation.satellites,
      timestamp: latestLocation.timestamp,
      deviceId: latestLocation.deviceId
    });
  } catch (err) {
    console.error('Get user GPS error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/gps/user/:userId/trail — Get GPS trail for specific user (protected)
app.get('/api/gps/user/:userId/trail', authMiddleware, async (req, res) => {
  try {
    const { userId } = req.params;
    const limit = Math.min(parseInt(req.query.limit) || 100, 500);

    // Users can only see their own trail, admins can see any
    if (req.user.role !== 'admin' && req.user.userId !== userId) {
      return res.status(403).json({ message: 'Access denied.' });
    }

    const trail = await GpsLocation.find({ userId })
      .sort({ timestamp: -1 })
      .limit(limit);

    res.json(trail.reverse());  // Return in chronological order
  } catch (err) {
    console.error('Get trail error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/gps/all/active — Get all active users' latest locations (admin only)
app.get('/api/gps/all/active', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }

    // Get latest location for each user — only records from the last 60 seconds
    const cutoff = new Date(Date.now() - 60 * 1000);
    const activeLocations = await GpsLocation.aggregate([
      { $match: { isLive: true, timestamp: { $gte: cutoff } } },
      { $sort: { timestamp: -1 } },
      { $group: {
          _id: '$userId',
          latitude: { $first: '$latitude' },
          longitude: { $first: '$longitude' },
          speed: { $first: '$speed' },
          altitude: { $first: '$altitude' },
          satellites: { $first: '$satellites' },
          timestamp: { $first: '$timestamp' },
          deviceId: { $first: '$deviceId' }
        }
      }
    ]);

    // Populate user info
    const populated = await Promise.all(
      activeLocations.map(async (loc) => {
        const user = await User.findById(loc._id).select('firstName lastName email phone');
        return {
          userId: loc._id,
          userName: user ? `${user.firstName} ${user.lastName}` : 'Unknown',
          email: user ? user.email : '',
          latitude: loc.latitude,
          longitude: loc.longitude,
          speed: loc.speed,
          altitude: loc.altitude,
          satellites: loc.satellites,
          timestamp: loc.timestamp,
          deviceId: loc.deviceId
        };
      })
    );

    res.json(populated);
  } catch (err) {
    console.error('Get active locations error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/gps/status — Is GPS module online? (last update within 30 seconds)
app.get('/api/gps/status', (req, res) => {
  if (!latestGps) return res.json({ online: false, hasFix: false, message: 'No GPS data received yet.' });
  const age = Date.now() - latestGps.timestamp;
  res.json({
    online: age < 30000,
    hasFix: latestGps.hasFix !== false,
    lastSeen: latestGps.timestamp,
    ageSeconds: Math.floor(age / 1000),
    lat: latestGps.lat,
    lon: latestGps.lon,
    satellites: latestGps.satellites,
    speed: latestGps.speed
  });
});

// DELETE /api/gps/history — Admin clears GPS trail
app.delete('/api/gps/history', authMiddleware, (req, res) => {
  if (req.user.role !== 'admin') return res.status(403).json({ message: 'Admin access required.' });
  gpsHistory = [];
  latestGps = null;
  console.log('🗑️  GPS history cleared by admin.');
  res.json({ message: 'GPS history cleared.' });
});

// ─── Stations Data & Routes ────────────────────────────
// Real KUET-area docking stations with GPS coordinates.
// In production, replace this with a MongoDB Station model.
const STATION_DATA = [
  { id:'S1', name:'KUET Main Gate',    address:'KU Engineering Road, Khulna', lat:22.89970, lon:89.50100, totalBikes:8,  availableBikes:8,  status:'available' },
  { id:'S2', name:'KUET Campus Hub',   address:'Inside KUET Campus',          lat:22.90318, lon:89.50348, totalBikes:6,  availableBikes:5,  status:'available' },
  { id:'S3', name:'Boyra Market',      address:'Boyra, Khulna',               lat:22.91520, lon:89.51430, totalBikes:5,  availableBikes:3,  status:'few'       },
  { id:'S4', name:'Fulbari Gate',      address:'Fulbari Road, Khulna',        lat:22.88200, lon:89.49000, totalBikes:8,  availableBikes:6,  status:'available' },
  { id:'S5', name:'Khulna Rail Gate',  address:'Railway Station Area, Khulna',lat:22.84300, lon:89.54500, totalBikes:4,  availableBikes:0,  status:'empty'     },
];

// Haversine distance in km (server-side helper)
function gpsDistKm(lat1, lon1, lat2, lon2) {
  const R = 6371;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a = Math.sin(dLat/2) ** 2 +
            Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
            Math.sin(dLon/2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

// GET /api/stations — Return all docking stations with coordinates
app.get('/api/stations', (req, res) => {
  res.json(STATION_DATA);
});

// GET /api/stations/nearby?lat=&lon= — Return stations sorted by distance from GPS position
app.get('/api/stations/nearby', (req, res) => {
  const { lat, lon } = req.query;
  if (!lat || !lon) return res.json(STATION_DATA);
  const uLat = parseFloat(lat), uLon = parseFloat(lon);
  if (isNaN(uLat) || isNaN(uLon)) return res.json(STATION_DATA);
  const sorted = STATION_DATA
    .map(s => ({ ...s, distanceKm: parseFloat(gpsDistKm(uLat, uLon, s.lat, s.lon).toFixed(3)) }))
    .sort((a, b) => a.distanceKm - b.distanceKm);
  res.json(sorted);
});


// POST /api/admin/create — Create a new admin account (requires existing admin token OR first-time setup secret)
app.post('/api/admin/create', async (req, res) => {
  try {
    const { firstName, lastName, email, phone, city, password, setupSecret } = req.body;

    // Allow either: a valid admin JWT OR the SETUP_SECRET env var for bootstrap
    const token = req.headers.authorization?.split(' ')[1];
    let authorized = false;

    if (token) {
      try {
        const decoded = jwt.verify(token, process.env.JWT_SECRET);
        if (decoded.role === 'admin') authorized = true;
      } catch {}
    }
    if (!authorized && setupSecret) {
      if (setupSecret === process.env.SETUP_SECRET) authorized = true;
    }

    if (!authorized) {
      return res.status(403).json({ message: 'Admin token or valid setup secret required.' });
    }

    if (!firstName || !lastName || !email || !phone || !city || !password) {
      return res.status(400).json({ message: 'All fields are required.' });
    }
    if (password.length < 6) {
      return res.status(400).json({ message: 'Password must be at least 6 characters.' });
    }

    const existing = await User.findOne({ email });
    if (existing) {
      return res.status(409).json({ message: 'An account with this email already exists.' });
    }

    const hashed = await bcrypt.hash(password, 12);
    const admin = await User.create({
      firstName, lastName, email, phone, city,
      password: hashed,
      plan: 'basic',
      role: 'admin',
      isActive: true
    });

    res.status(201).json({
      message: 'Admin account created successfully.',
      admin: { id: admin._id, email: admin.email, firstName: admin.firstName, lastName: admin.lastName }
    });
  } catch (err) {
    console.error('Create admin error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// PATCH /api/admin/promote — Promote any existing user to admin (requires admin token)
app.patch('/api/admin/promote', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    const { email } = req.body;
    if (!email) return res.status(400).json({ message: 'Email is required.' });

    const user = await User.findOneAndUpdate({ email }, { role: 'admin' }, { new: true }).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });

    res.json({ message: `${user.email} has been promoted to admin.`, user });
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/admin/users — List all users (admin only)
app.get('/api/admin/users', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    const users = await User.find().select('-password').sort({ createdAt: -1 });
    res.json(users);
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// ─── Ride Schema ──────────────────────────────────────
const rideSchema = new mongoose.Schema({
  userId:      { type: mongoose.Schema.Types.ObjectId, ref: 'User', required: true },
  rfidUid:     { type: String, required: true },
  bikeId:      { type: String, required: true },
  stationStart:{ type: String, required: true },
  stationEnd:  { type: String, default: null },
  startTime:   { type: Date, default: Date.now },
  endTime:     { type: Date, default: null },
  duration:    { type: String, default: null },   // "00:14:32"
  distanceKm:  { type: Number, default: 0 },
  fare:        { type: Number, default: 0 },
  status:      { type: String, enum: ['active', 'completed', 'error'], default: 'active' },
  pendingCommand: { type: String, default: null }
});
const Ride = mongoose.model('Ride', rideSchema);

// ─── Transaction Schema ───────────────────────────────
const transactionSchema = new mongoose.Schema({
  userId:      { type: mongoose.Schema.Types.ObjectId, ref: 'User', required: true },
  type:        { type: String, enum: ['credit', 'debit'], required: true },
  title:       { type: String, required: true },
  description: { type: String, default: '' },
  amount:      { type: Number, required: true },
  newBalance:  { type: Number, default: 0 },
  reference:   { type: String, default: '' },  // e.g., rideId, stripeSessionId
  timestamp:   { type: Date, default: Date.now }
});
const Transaction = mongoose.model('Transaction', transactionSchema);

// POST /api/transactions — Save a new transaction
app.post('/api/transactions', authMiddleware, async (req, res) => {
  try {
    const { type, title, amount, description, newBalance, reference } = req.body;
    if (!type || !title || !amount) {
      console.log('❌ Missing fields in transaction request');
      return res.status(400).json({ message: 'type, title, and amount are required.' });
    }
    
    const transaction = await Transaction.create({
      userId: req.user.userId,
      type,
      title,
      description: description || '',
      amount,
      newBalance: newBalance || 0,
      reference: reference || ''
    });
    
    console.log(`💳 Transaction saved: user=${req.user.userId} type=${type} amount=${amount} title=${title}`);
    res.status(201).json(transaction);
  } catch (err) {
    console.error('Transaction create error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/transactions — Get user's transaction history
app.get('/api/transactions', authMiddleware, async (req, res) => {
  try {
    const transactions = await Transaction.find({ userId: req.user.userId })
      .sort({ timestamp: -1 })
      .limit(500);
    console.log(`📋 Fetching transactions: user=${req.user.userId} count=${transactions.length}`);
    res.json(transactions);
  } catch (err) {
    console.error('Transactions fetch error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// POST /api/rides/start — Dashboard calls this when user clicks "Start Ride"
// Creates ride in DB and sets a pending unlock command for the ESP32 to pick up.
app.post('/api/rides/start', authMiddleware, async (req, res) => {
  try {
    const { stationId, bikeId } = req.body;
    if (!stationId || !bikeId) {
      return res.status(400).json({ message: 'stationId and bikeId are required.' });
    }
    // Look up user from auth token — no need for dashboard to send rfidUid
    const user = await User.findById(req.user.userId).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });
    if (!user.rfidUid) return res.status(400).json({ message: 'No RFID card linked to your account.' });
    const rfidUid = user.rfidUid;

    // Check for already active ride
    const existing = await Ride.findOne({ userId: user._id, status: 'active' });
    if (existing) {
      // Re-issue the unlock command — persist in DB so it survives server restarts
      await Ride.findByIdAndUpdate(existing._id, { pendingCommand: 'unlock' });
      console.log(`🔄 Re-issued unlock command for ${bikeId} (ride already active: ${existing._id})`);
      return res.status(200).json({ message: 'Unlock command re-issued.', rideId: existing._id });
    }

    const ride = await Ride.create({
      userId: user._id,
      rfidUid: rfidUid.toUpperCase(),
      bikeId,
      stationStart: stationId,
      pendingCommand: 'unlock'   // ← persisted in DB, survives Render restarts
    });

    console.log(`🔑 Ride queued (DB) for ${bikeId} | rideId: ${ride._id} | user: ${user.firstName} ${user.lastName} — waiting for RFID tap to unlock`);

    res.status(201).json({
      message: 'Bike selected. Tap your RFID card on the bike to unlock.',
      rideId: ride._id,
      user: { firstName: user.firstName, plan: user.plan, walletBalance: user.walletBalance }
    });
  } catch (err) {
    console.error('Start ride error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/bikes/:bikeId/command — ESP32 polls this to get pending commands (unlock OR lock)
// Returns the command once and clears it (one-shot delivery).
app.get('/api/bikes/:bikeId/command', async (req, res) => {
  const bikeId = req.params.bikeId;

  // ── 1. Check for pending LOCK command (set when website ends a ride) ──
  if (bikeLockCommands[bikeId]) {
    const info = bikeLockCommands[bikeId];
    delete bikeLockCommands[bikeId];
    console.log(`📡 Lock command delivered to ${bikeId}`);
    return res.json({ command: 'lock', rideId: info.rideId || '' });
  }

  // ── 2. Check for pending UNLOCK command (set when website starts a ride) ──
  const ride = await Ride.findOne({ bikeId, pendingCommand: 'unlock', status: 'active' });
  if (!ride) {
    return res.status(204).send();   // no pending command
  }
  // Clear so the command is only delivered once
  await Ride.findByIdAndUpdate(ride._id, { pendingCommand: null });
  console.log(`📡 Unlock command delivered to ${bikeId}: rfid=${ride.rfidUid} rideId=${ride._id}`);
  res.json({ command: 'unlock', rfidUid: ride.rfidUid, rideId: ride._id.toString() });
});

// POST /api/rides/end/user — Website (JWT auth) ends an active ride and queues solenoid lock
app.post('/api/rides/end/user', authMiddleware, async (req, res) => {
  try {
    const { rideId, stationId } = req.body;

    const user = await User.findById(req.user.userId).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });

    // Find active ride by rideId (if given) or latest active ride for this user
    let ride;
    if (rideId) {
      ride = await Ride.findById(rideId);
      if (!ride || ride.userId.toString() !== user._id.toString()) {
        return res.status(404).json({ message: 'Ride not found or access denied.' });
      }
    } else {
      ride = await Ride.findOne({ userId: user._id, status: 'active' }).sort({ startTime: -1 });
    }
    if (!ride) return res.status(404).json({ message: 'No active ride found.' });

    const endTime = new Date();
    const elapsedSec = Math.floor((endTime - ride.startTime) / 1000);
    const mins = Math.floor(elapsedSec / 60);
    const secs = elapsedSec % 60;
    const duration = `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}`;
    const distanceKm = parseFloat((elapsedSec * 0.00278).toFixed(2));
    const fare = Math.max(Math.ceil(elapsedSec / 60) * 2, 2);

    const newBalance = Math.max((user.walletBalance || 0) - fare, 0);
    await User.findByIdAndUpdate(user._id, { walletBalance: newBalance });

    await Ride.findByIdAndUpdate(ride._id, {
      status: 'completed',
      endTime,
      stationEnd: stationId || 'Unknown',
      duration,
      distanceKm,
      fare
    });

    // ── Queue lock command so ESP32 locks solenoid on next poll ──
    bikeLockCommands[ride.bikeId] = { rideId: ride._id.toString() };
    console.log(`🔒 Lock command queued (web) for ${ride.bikeId} | user: ${user.firstName}`);
    // ── Also publish via MQTT for instant delivery ──
    mqttPublish(`kuettrack/${ride.bikeId}/cmd`, { command: 'lock', rideId: ride._id.toString() });

    res.json({
      message: 'Ride ended.',
      duration,
      distanceKm,
      fare,
      newBalance
    });
  } catch (err) {
    console.error('End ride (user) error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// POST /api/rides/end — ESP-32 calls when user taps card to return bike
app.post('/api/rides/end', async (req, res) => {
  try {
    const { rfidUid, rideId, stationId } = req.body;
    if (!rfidUid) return res.status(400).json({ message: 'rfidUid is required.' });

    const user = await User.findOne({ rfidUid, isActive: true });
    if (!user) return res.status(404).json({ message: 'RFID card not recognized.' });

    // Find the active ride (by rideId if given, else latest active)
    let ride;
    if (rideId) {
      ride = await Ride.findById(rideId);
    } else {
      ride = await Ride.findOne({ userId: user._id, status: 'active' }).sort({ startTime: -1 });
    }
    if (!ride) return res.status(404).json({ message: 'No active ride found.' });

    const endTime = new Date();
    const elapsedSec = Math.floor((endTime - ride.startTime) / 1000);
    const mins = Math.floor(elapsedSec / 60);
    const secs = elapsedSec % 60;
    const duration = `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}`;
    const distanceKm = parseFloat((elapsedSec * 0.00278).toFixed(2)); // ~10 km/h average
    const fare = Math.max(Math.ceil(elapsedSec / 60) * 2, 2); // ৳2/min minimum ৳2

    // Deduct fare from wallet
    const newBalance = Math.max((user.walletBalance || 0) - fare, 0);
    await User.findByIdAndUpdate(user._id, { walletBalance: newBalance });

    // Update ride record
    await Ride.findByIdAndUpdate(ride._id, {
      status: 'completed',
      endTime,
      stationEnd: stationId || 'Unknown',
      duration,
      distanceKm,
      fare
    });

    // ── Queue lock command so ESP32 locks solenoid on next poll ──
    bikeLockCommands[ride.bikeId] = { rideId: ride._id.toString() };
    console.log(`🔒 Lock command queued for ${ride.bikeId}`);
    mqttPublish(`kuettrack/${ride.bikeId}/cmd`, { command: 'lock', rideId: ride._id.toString() });

    res.json({
      message: 'Ride ended.',
      duration: `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}`,
      distanceKm,
      fare,
      newBalance
    });
  } catch (err) {
    console.error('End ride error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});


// GET /api/rides/active — Check if current user has an active ride
app.get("/api/rides/active", authMiddleware, async (req, res) => {
  try {
    const ride = await Ride.findOne({ userId: req.user.userId, status: "active" }).sort({ startTime: -1 });
    if (!ride) return res.json({ active: false });
    res.json({
      active: true,
      rideId: ride._id,
      bikeId: ride.bikeId,
      stationStart: ride.stationStart,
      startTime: ride.startTime
    });
  } catch (err) {
    console.error("Active ride check error:", err);
    res.status(500).json({ message: "Server error." });
  }
});
// GET /api/rides — List all rides (admin only)
app.get('/api/rides', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') return res.status(403).json({ message: 'Admin access required.' });
    const rides = await Ride.find().populate('userId','firstName lastName email').sort({ startTime: -1 }).limit(200);
    res.json(rides);
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/rides/user/:userId — User's own ride history
app.get('/api/rides/user/:userId', authMiddleware, async (req, res) => {
  try {
    if (req.user.userId !== req.params.userId && req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Access denied.' });
    }
    const rides = await Ride.find({ userId: req.params.userId }).sort({ startTime: -1 });
    res.json(rides);
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/admin/rfid-logins — Get all RFID login history (admin only)
app.get('/api/admin/rfid-logins', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    
    // Optional: Get logins from last 24 hours or all logins
    const logins = await RfidLogin.find()
      .populate('userId', 'firstName lastName email phone')
      .sort({ timestamp: -1 })
      .limit(500);
    
    res.json(logins);
  } catch (err) {
    console.error('RFID logins fetch error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/admin/rfid-logins/today — Get today's RFID login statistics
app.get('/api/admin/rfid-logins/today', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    
    const today = new Date();
    today.setHours(0, 0, 0, 0);
    const tomorrow = new Date(today);
    tomorrow.setDate(tomorrow.getDate() + 1);
    
    const logins = await RfidLogin.find({
      timestamp: { $gte: today, $lt: tomorrow }
    }).populate('userId', 'firstName lastName email');
    
    const successful = logins.filter(l => l.status === 'success').length;
    const failed = logins.filter(l => l.status === 'invalid_card').length;
    const errors = logins.filter(l => l.status === 'error').length;
    
    res.json({
      date: today.toDateString(),
      total: logins.length,
      successful,
      failed,
      errors,
      logins: logins.sort((a, b) => b.timestamp - a.timestamp)
    });
  } catch (err) {
    console.error('RFID stats error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/admin/rfid-logins/user/:userId — Get RFID login history for specific user
app.get('/api/admin/rfid-logins/user/:userId', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    
    const logins = await RfidLogin.find({ userId: req.params.userId })
      .sort({ timestamp: -1 })
      .limit(200);
    
    res.json(logins);
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// GET /api/admin/rfid-stats — Overall RFID statistics
app.get('/api/admin/rfid-stats', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    
    const total = await RfidLogin.countDocuments();
    const successful = await RfidLogin.countDocuments({ status: 'success' });
    const failed = await RfidLogin.countDocuments({ status: 'invalid_card' });
    const errors = await RfidLogin.countDocuments({ status: 'error' });
    
    // Last 7 days trend
    const sevenDaysAgo = new Date();
    sevenDaysAgo.setDate(sevenDaysAgo.getDate() - 7);
    
    const recentLogins = await RfidLogin.find({
      timestamp: { $gte: sevenDaysAgo }
    }).countDocuments();
    
    res.json({
      total,
      successful,
      failed,
      errors,
      successRate: ((successful / (total || 1)) * 100).toFixed(2) + '%',
      recentLogins,
      averagePerDay: (recentLogins / 7).toFixed(2)
    });
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// PATCH /api/admin/users/:id — Update user plan, wallet, suspend/restore (admin only)
app.patch('/api/admin/users/:id', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    const { plan, walletBalance, isActive } = req.body;
    const updates = {};
    if (plan !== undefined) updates.plan = plan;
    if (walletBalance !== undefined) updates.walletBalance = walletBalance;
    if (isActive !== undefined) updates.isActive = isActive;

    const user = await User.findByIdAndUpdate(req.params.id, updates, { new: true }).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });
    res.json({ message: 'User updated.', user });
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});

// DELETE /api/admin/users/:id/rfid — Remove RFID assignment from user (admin only)
app.delete('/api/admin/users/:id/rfid', authMiddleware, async (req, res) => {
  try {
    if (req.user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required.' });
    }
    const user = await User.findByIdAndUpdate(req.params.id, { rfidUid: null }, { new: true }).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });
    res.json({ message: 'RFID removed.', user });
  } catch (err) {
    res.status(500).json({ message: 'Server error.' });
  }
});


// ─── Direct Wallet Top-Up (no Stripe required) ──────────
// POST /api/wallet/topup — instantly credits user wallet (demo / non-Stripe flow)
// Used when STRIPE_SECRET_KEY is not configured (free-tier / Bangladesh users).
app.post('/api/wallet/topup', authMiddleware, async (req, res) => {
  try {
    const { amount } = req.body;
    if (!amount || isNaN(amount) || amount < 10 || amount > 10000) {
      return res.status(400).json({ message: 'Amount must be between ৳10 and ৳10,000.' });
    }
    const topup = parseInt(amount, 10);
    const user = await User.findByIdAndUpdate(
      req.user.userId,
      { $inc: { walletBalance: topup } },
      { new: true }
    ).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });

    // Log as a transaction
    await Transaction.create({
      userId: user._id,
      type: 'credit',
      title: 'Wallet Top-Up',
      description: `Direct top-up of ৳${topup}`,
      amount: topup,
      newBalance: user.walletBalance,
      reference: 'direct-topup'
    });

    console.log(`💰 Wallet topped up: user=${user.email} +৳${topup} → ৳${user.walletBalance}`);
    res.json({ success: true, addedAmount: topup, newBalance: user.walletBalance });
  } catch (err) {
    console.error('Wallet topup error:', err);
    res.status(500).json({ message: 'Server error.' });
  }
});

// ─── Stripe Payment Routes ─────────────────────────────

// POST /api/payment/create-checkout-session
// Creates a Stripe Checkout session for wallet top-up
app.post('/api/payment/create-checkout-session', authMiddleware, async (req, res) => {
  const s = getStripe();
  if (!s) return res.status(503).json({ message: 'Payment system not configured. Add STRIPE_SECRET_KEY to .env and run: npm install stripe' });
  try {
    const { amount } = req.body;
    if (!amount || amount < 10) {
      return res.status(400).json({ message: 'Minimum top-up amount is ৳10.' });
    }

    const user = await User.findById(req.user.userId).select('-password');
    if (!user) return res.status(404).json({ message: 'User not found.' });

    // Use environment variable for deployment compatibility
    const FRONTEND_URL = process.env.FRONTEND_URL || 'http://localhost:5000';
    console.log(`📍 Payment session: amount=৳${amount}, redirect=${FRONTEND_URL}/dashboard.html`);

    const session = await s.checkout.sessions.create({
      payment_method_types: ['card'],
      mode: 'payment',
      customer_email: user.email,
      line_items: [
        {
          price_data: {
            currency: 'usd',  // BDT not supported by Stripe Checkout
            product_data: {
              name: 'KuetTrack Wallet Top-Up',
              description: `Add ৳${amount} to your KuetTrack wallet`,
            },
            unit_amount: amount, // treat 1 cent = 1 taka (test mode only)
          },
          quantity: 1,
        },
      ],
      metadata: {
        userId: user._id.toString(),
        topupAmount: amount.toString(),
      },
      // Embed a short-lived payment-return token so the dashboard always
      // restores the correct user session regardless of what is in localStorage.
      success_url: `${FRONTEND_URL}/dashboard.html?topup=success&amount=${amount}&session_id={CHECKOUT_SESSION_ID}&prt=${jwt.sign({ userId: user._id, email: user.email, role: user.role }, process.env.JWT_SECRET, { expiresIn: '10m' })}`,
      cancel_url:  `${FRONTEND_URL}/dashboard.html?topup=cancel`,
    });

    res.json({ url: session.url, sessionId: session.id });
  } catch (err) {
    console.error('Stripe session error:', err.message, err);
    res.status(500).json({ message: `Could not create payment session: ${err.message}` });
  }
});

// POST /api/payment/webhook
// Stripe webhook — credits wallet after confirmed payment
// ⚠️  Register this URL in your Stripe Dashboard → Webhooks
app.post('/api/payment/webhook', express.raw({ type: 'application/json' }), async (req, res) => {
  const s = getStripe();
  if (!s) return res.status(503).json({ message: 'Stripe not configured.' });
  const sig = req.headers['stripe-signature'];
  let event;

  try {
    event = s.webhooks.constructEvent(req.body, sig, process.env.STRIPE_WEBHOOK_SECRET);
  } catch (err) {
    console.error('Webhook signature error:', err.message);
    return res.status(400).send(`Webhook Error: ${err.message}`);
  }

  if (event.type === 'checkout.session.completed') {
    const session = event.data.object;
    const userId     = session.metadata?.userId;
    const topupAmt   = parseInt(session.metadata?.topupAmount || '0', 10);

    if (userId && topupAmt > 0) {
      try {
        await User.findByIdAndUpdate(userId, {
          $inc: { walletBalance: topupAmt }
        });
        console.log(`✅ Wallet credited: user=${userId} amount=৳${topupAmt}`);
      } catch (err) {
        console.error('Wallet credit error:', err);
      }
    }
  }

  res.json({ received: true });
});

// GET /api/payment/verify-session/:sessionId
// Verify payment and credit wallet — idempotent (safe to call multiple times)
app.get('/api/payment/verify-session/:sessionId', authMiddleware, async (req, res) => {
  const s = getStripe();
  if (!s) return res.status(503).json({ message: 'Stripe not configured.' });
  try {
    const sessionId = req.params.sessionId;
    const session = await s.checkout.sessions.retrieve(sessionId);

    if (session.payment_status === 'paid') {
      const userId   = session.metadata?.userId;
      const topupAmt = parseInt(session.metadata?.topupAmount || '0', 10);

      if (userId && topupAmt > 0) {
        // Only credit once — check if this sessionId was already applied
        const alreadyCredited = await User.findOne({ _id: userId, creditedSessions: sessionId });
        if (!alreadyCredited) {
          await User.findByIdAndUpdate(userId, {
            $inc:  { walletBalance: topupAmt },
            $push: { creditedSessions: sessionId }
          });
          console.log(`✅ Wallet credited: user=${userId} amount=৳${topupAmt}`);
        } else {
          console.log(`ℹ️  Session ${sessionId} already credited — skipping double credit.`);
        }

        const updatedUser = await User.findById(userId).select('-password');
        return res.json({ paid: true, walletBalance: updatedUser.walletBalance });
      }
    }

    const user = await User.findById(req.user.userId).select('-password');
    res.json({ paid: false, walletBalance: user?.walletBalance || 0 });
  } catch (err) {
    console.error('Verify session error:', err);
    res.status(500).json({ message: 'Could not verify session.' });
  }
});

// ─── Public System Health (no auth required) ─────────
app.get('/api/system/health', async (req, res) => {
  try {
    // Check if DB is connected
    const dbStatus = mongoose.connection.readyState === 1 ? 'connected' : 'disconnected';
    
    // Count recent RFID activity (last hour)
    const oneHourAgo = new Date(Date.now() - 60 * 60 * 1000);
    const recentActivity = await RfidLogin.countDocuments({ timestamp: { $gte: oneHourAgo } });
    
    res.json({
      status: 'online',
      database: dbStatus,
      rfidSystemActive: recentActivity > 0,
      timestamp: new Date()
    });
  } catch (err) {
    console.error('Health check error:', err);
    res.status(500).json({ status: 'error', message: 'Health check failed' });
  }
});

// ─── Serve frontend HTML files statically ────────────
// HTML files live in the same directory as server.js (project root)
// Works both locally and on Render without any folder restructuring
const path = require('path');

// Check if a 'public' or 'Public' subfolder exists; fall back to root
const fs = require('fs');
const publicDir = fs.existsSync(path.join(__dirname, 'public'))
  ? path.join(__dirname, 'public')
  : fs.existsSync(path.join(__dirname, 'Public'))
    ? path.join(__dirname, 'Public')
    : __dirname;

app.use(express.static(publicDir));
app.get('/', (req, res) => {
  res.sendFile(path.join(publicDir, 'index.html'));
});

// ─── Start Server ─────────────────────────────────────
const PORT = process.env.PORT || 5000;
const os = require('os');
function getLanIp() {
  const nets = os.networkInterfaces();
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) return net.address;
    }
  }
  return 'unknown';
}
app.listen(PORT, '0.0.0.0', () => {
  const lanIp = getLanIp();
  console.log(`
🚲 KuetTrack running! Local: http://localhost:${PORT}  Network: http://${lanIp}:${PORT}`);
  console.log(`📡 Set ESP32 SERVER_URL = "http://${lanIp}:${PORT}/api/gps/update"`);
  console.log(`🌐 Frontend: http://${lanIp}:${PORT}/dashboard.html
`);
});
