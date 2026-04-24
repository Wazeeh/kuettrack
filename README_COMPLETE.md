# 🚲 **VeloTrack - Smart RFID Bike Rental System**

> A complete, production-ready bike rental platform with RFID hardware integration, real-time authentication, and professional admin monitoring.

---

## 📸 **System Features**

### ✨ **Core Features**
- ✅ **RFID Card Authentication** - Tap to instantly authenticate and access bikes
- ✅ **Hardware Integration** - ESP-32 + RC522 RFID reader with live feedback
- ✅ **Real-time Monitoring** - Admin dashboard tracks all RFID logins
- ✅ **Automated Billing** - Smart fare calculation based on ride duration
- ✅ **Secure Authentication** - JWT tokens with role-based access
- ✅ **Professional UI** - Responsive, modern design for all devices
- ✅ **User Profiles** - Ride history, wallet management, RFID assignment
- ✅ **Admin Controls** - Complete system monitoring and user management

### 🔒 **Security Features**
- Encrypted password storage (bcryptjs)
- JWT-based authentication (7-day expiration)
- Role-based access control (Admin/User)
- RFID card validation & duplicate prevention
- IP tracking and audit logging
- CORS protection

---

## 🏗️ **System Architecture**

```
┌─────────────────────────────────────────────────────────────────┐
│                        Admin Dashboard                           │
│              (RFID Monitoring & User Management)                │
└─────────────────────────────────────────────────────────────────┘
                              ↑
┌─────────────────────────────────────────────────────────────────┐
│                     Backend API (Node.js/Express)               │
│  ├─ Authentication & JWT                                        │
│  ├─ RFID Login Tracking                                         │
│  ├─ Ride Management                                             │
│  └─ User Management                                             │
└─────────────────────────────────────────────────────────────────┘
     ↑                                              ↑
     │                                              │
┌────────────────┐                      ┌──────────────────────┐
│   User Portal  │                      │  RFID Hardware (ESP32)│
│  (Web Browser) │                      │  ├─ RC522 Reader    │
└────────────────┘                      │  ├─ LCD Display     │
                                        │  ├─ Status LEDs     │
                                        │  └─ Buzzer          │
                                        └──────────────────────┘
                                              ↑
                                        Bike at Station
```

---

## 📦 **Project Structure**

```
bike-rental/
├── server.js                          # Main backend API
├── esp32_rfid_controller.ino          # Arduino firmware for ESP-32
├── package.json                       # Node dependencies
├── SETUP_AND_INTEGRATION_GUIDE.md     # Detailed setup guide
├── QUICK_REFERENCE.md                 # Quick start reference
├── README.md                          # This file
│
└── Public/                            # Frontend files
    ├── index.html                     # Home page (new)
    ├── index_new.html                 # Home page template
    ├── login.html                     # User login
    ├── login_new.html                 # Login template
    ├── register.html                  # User registration
    ├── dashboard.html                 # User dashboard
    ├── dashboard_new.html             # Dashboard template
    ├── admin-dashboard.html           # Admin panel
    ├── admin-dashboard_new.html       # Admin template
    ├── rfid-setup.html                # RFID setup guide
    └── [other files]
```

---

## 🚀 **Quick Start**

### Prerequisites
- Node.js 14+
- MongoDB Atlas account
- Arduino IDE (for ESP-32)
- ESP-32 development board
- RC522 RFID reader module

### Step 1: Install Backend
```bash
cd bike-rental
npm install
```

### Step 2: Configure Environment
Create `.env` file:
```env
MONGODB_URI=mongodb+srv://user:pass@cluster.mongodb.net/velotrack
JWT_SECRET=your_secret_key_here
ADMIN_EMAIL=admin@velotrack.com
ADMIN_PASSWORD=Admin@1234
PORT=5000
```

### Step 3: Start Backend
```bash
node server.js
```

### Step 4: Configure ESP-32
1. Open `esp32_rfid_controller.ino` in Arduino IDE
2. Update WiFi credentials:
   ```cpp
   #define WIFI_SSID "YOUR_WIFI"
   #define WIFI_PASSWORD "YOUR_PASSWORD"
   #define API_BASE_URL "http://YOUR_BACKEND_IP:5000"
   ```
3. Upload to ESP-32

### Step 5: Access System
- **Home**: `http://localhost:5000/index.html`
- **Login**: `http://localhost:5000/login.html`
- **Admin**: `http://localhost:5000/admin-dashboard.html` (use admin credentials)

---

## 📱 **User Flow**

### 1️⃣ **User Registration**
```
Navigate to /register.html
↓
Fill in personal details
↓
Create account with password
↓
JWT token generated
↓
Redirected to user dashboard
```

### 2️⃣ **RFID Card Assignment**
```
Admin assigns card via database
↓
User visits admin dashboard
↓
Shows "RFID Card Status: Assigned"
↓
Card UID displayed (A1B2C3D4...)
```

### 3️⃣ **Tap Card at Station**
```
User taps RFID card on reader
↓
ESP-32 reads card UID
↓
Sends to Backend: GET /api/auth/user-by-rfid/UID
↓
Backend verifies user & logs attempt
↓
IF Valid: Green LED + 2 beeps
IF Invalid: Red LED + 3 beeps
```

### 4️⃣ **Start Ride**
```
ESP-32 shows: "Auth Success! Riding Started"
↓
Backend creates ride record
↓
User can't start another ride until completed
↓
Ride timestamp, location, bike ID recorded
```

### 5️⃣ **End Ride**
```
User taps card again at return station
↓
ESP-32 recognizes same card
↓
Sends END request with ride ID
↓
Backend calculates:
  - Duration (minutes)
  - Distance (km)
  - Fare (Duration × ৳2/min)
↓
Wallet automatically deducted
↓
Ride marked as "completed"
```

### 6️⃣ **Admin Monitoring**
```
Admin logs in with admin@velotrack.com
↓
Dashboard shows:
  ✓ Total RFID logins
  ✓ Successful vs failed
  ✓ Success rate %
  ✓ Daily trends
↓
RFID Logins tab shows:
  - Timestamp of each tap
  - User who tapped
  - Card UID
  - Success/Failed status
  - IP address
↓
Can filter by date, user, or status
```

---

## 💻 **API Documentation**

### Authentication Endpoints

#### Register User
```
POST /api/auth/register
Content-Type: application/json

{
  "firstName": "John",
  "lastName": "Doe",
  "email": "john@example.com",
  "phone": "+8801234567890",
  "city": "Dhaka",
  "password": "password123",
  "plan": "basic"
}

Response: 201 Created
{
  "message": "Account created successfully!",
  "token": "eyJhbGciOiJIUzI1NiIs...",
  "user": { ... }
}
```

#### Login User
```
POST /api/auth/login
Content-Type: application/json

{
  "email": "john@example.com",
  "password": "password123"
}

Response: 200 OK
{
  "message": "Login successful!",
  "token": "eyJhbGciOiJIUzI1NiIs...",
  "user": { ... }
}
```

#### Get User Profile (Protected)
```
GET /api/auth/me
Authorization: Bearer YOUR_JWT_TOKEN

Response: 200 OK
{
  "_id": "507f1f77bcf86cd799439011",
  "firstName": "John",
  "lastName": "Doe",
  "email": "john@example.com",
  "rfidUid": "A1B2C3D4",
  "walletBalance": 1000,
  "plan": "basic",
  "isActive": true
}
```

### RFID Endpoints

#### Verify RFID Card (Called by ESP-32)
```
GET /api/auth/user-by-rfid/A1B2C3D4

Response: 200 OK (Valid Card)
{
  "authorized": true,
  "user": {
    "id": "507f1f77bcf86cd799439011",
    "firstName": "John",
    "lastName": "Doe",
    "plan": "basic",
    "walletBalance": 1000
  }
}

Response: 404 (Invalid Card)
{
  "authorized": false,
  "message": "RFID card not recognized."
}
```

#### Get RFID Login History (Admin Only)
```
GET /api/admin/rfid-logins
Authorization: Bearer ADMIN_JWT_TOKEN

Response: 200 OK
[
  {
    "_id": "507f1f77bcf86cd799439011",
    "userId": { "firstName": "John", "lastName": "Doe", ... },
    "rfidUid": "A1B2C3D4",
    "status": "success",
    "timestamp": "2026-04-17T10:45:00Z",
    "ipAddress": "192.168.1.100"
  },
  ...
]
```

#### Get RFID Statistics (Admin Only)
```
GET /api/admin/rfid-stats
Authorization: Bearer ADMIN_JWT_TOKEN

Response: 200 OK
{
  "total": 245,
  "successful": 234,
  "failed": 8,
  "errors": 3,
  "successRate": "95.51%",
  "recentLogins": 156,
  "averagePerDay": "22.29"
}
```

### Ride Endpoints

#### Start Ride (Called by ESP-32)
```
POST /api/rides/start
Content-Type: application/json

{
  "rfidUid": "A1B2C3D4",
  "stationId": "STATION-001",
  "bikeId": "BIKE-001"
}

Response: 201 Created
{
  "message": "Ride started.",
  "rideId": "507f1f77bcf86cd799439011",
  "user": {
    "firstName": "John",
    "plan": "basic",
    "walletBalance": 1000
  }
}
```

#### End Ride (Called by ESP-32)
```
POST /api/rides/end
Content-Type: application/json

{
  "rfidUid": "A1B2C3D4",
  "rideId": "507f1f77bcf86cd799439011",
  "stationId": "STATION-002"
}

Response: 200 OK
{
  "message": "Ride ended.",
  "duration": "00:15:32",
  "distanceKm": 2.5,
  "fare": 30,
  "newBalance": 970
}
```

---

## ⚙️ **Hardware Setup**

### Required Components
- ESP-32 Dev Board
- RC522 RFID Reader
- 16x2 LCD (I2C)
- Green & Red LEDs
- Buzzer
- USB Power

### Pin Connections
```
ESP-32 (GPIO)  → RC522 Module
GPIO 22        → RST
GPIO 21        → SS (CS)
GPIO 23        → MOSI
GPIO 19        → MISO
GPIO 18        → SCK

ESP-32 (GPIO)  → Peripherals
GPIO 21        → SDA (LCD)
GPIO 22        → SCL (LCD)
GPIO 32        → Green LED
GPIO 33        → Red LED
GPIO 25        → Buzzer
```

### User Feedback
- **Green LED + 2 beeps**: Authentication successful
- **Red LED + 3 beeps**: Authentication failed
- **LCD Display**: Shows status messages
- **Serial Monitor**: Debug information

---

## 🔐 **Default Credentials**

**Admin Account** (Change after first login!):
```
Email: admin@velotrack.com
Password: Admin@1234
```

---

## 📊 **Database Schema**

### Users Collection
```javascript
{
  firstName: String,
  lastName: String,
  email: String (unique),
  phone: String,
  city: String,
  password: String (hashed),
  plan: String (basic/monthly/student),
  rfidUid: String (optional),
  walletBalance: Number,
  isActive: Boolean,
  role: String (user/admin),
  createdAt: Date
}
```

### RFID Logins Collection
```javascript
{
  userId: ObjectId (ref: User),
  rfidUid: String,
  status: String (success/invalid_card/error),
  stationId: String,
  ipAddress: String,
  timestamp: Date,
  deviceInfo: String
}
```

### Rides Collection
```javascript
{
  userId: ObjectId (ref: User),
  rfidUid: String,
  bikeId: String,
  stationStart: String,
  stationEnd: String,
  startTime: Date,
  endTime: Date,
  duration: String (HH:MM:SS),
  distanceKm: Number,
  fare: Number,
  status: String (active/completed/error)
}
```

---

## 🧪 **Testing**

### Test ESP-32 Connection
```bash
# Check if ESP-32 can reach backend
curl http://localhost:5000/api/auth/user-by-rfid/TEST123
# Expected: 404 (card not found)
```

### Test RFID Card
```bash
# Register user and assign RFID UID: A1B2C3D4
curl http://localhost:5000/api/auth/user-by-rfid/A1B2C3D4
# Expected: 200 with user data
```

### Check Admin Access
```bash
# Get admin stats
curl -H "Authorization: Bearer ADMIN_TOKEN" \
     http://localhost:5000/api/admin/rfid-stats
```

---

## 🐛 **Troubleshooting**

| Problem | Solution |
|---------|----------|
| ESP-32 WiFi fails | Check SSID/password in code, ensure router is on 2.4GHz |
| RFID not detected | Verify RC522 wiring, check RST/SS pins match code |
| Backend 500 error | Check MongoDB connection string, ensure cluster whitelist IP |
| Login fails | Verify user exists in database, check password |
| Admin stats empty | RFID logs created on first authentication attempt |
| Dashboard slow | Add indexes to MongoDB collections |

---

## 📝 **File Descriptions**

### Backend
- **server.js** - Main Express API server with all routes and database models

### Frontend
- **index_new.html** - Professional home page with features & testimonials
- **login_new.html** - User login with RFID status indicator
- **dashboard_new.html** - User dashboard with ride history & wallet
- **admin-dashboard_new.html** - Admin panel with RFID monitoring

### Hardware
- **esp32_rfid_controller.ino** - Complete Arduino firmware with WiFi, RFID, LCD

### Documentation
- **SETUP_AND_INTEGRATION_GUIDE.md** - Comprehensive setup instructions
- **QUICK_REFERENCE.md** - Quick configuration reference

---

## 🚀 **Deployment**

### Production Recommendations
1. Use environment-specific `.env` files
2. Enable HTTPS/SSL certificates
3. Set up database backups
4. Implement rate limiting on API
5. Use production-grade ESP-32 settings
6. Monitor system logs
7. Set up automated tests
8. Use Docker for containerization

### Scaling
- Use MongoDB Atlas for cloud storage
- Deploy backend on AWS/Heroku/DigitalOcean
- Use CDN for frontend assets
- Implement caching layer
- Set up load balancing for multiple ESP-32 units

---

## 📞 **Support**

For detailed setup instructions, see: **SETUP_AND_INTEGRATION_GUIDE.md**

For quick reference, see: **QUICK_REFERENCE.md**

---

## 📄 **License**

This project is provided as-is for bike rental system implementations.

---

## 🎉 **Get Started Now!**

```bash
# 1. Install dependencies
npm install

# 2. Create .env file with your MongoDB URI
# 3. Start the server
node server.js

# 4. Visit http://localhost:5000
# 5. Login with admin@velotrack.com / Admin@1234
```

**Welcome to VeloTrack! Happy Biking! 🚲**
