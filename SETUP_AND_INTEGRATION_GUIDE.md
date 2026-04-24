
# VeloTrack - RFID Smart Bike Rental System
## Complete Setup & Integration Guide

---

## 📋 Project Overview

VeloTrack is a professional RFID-enabled bike rental system featuring:
- **ESP-32 RFID Hardware Integration** - Tap cards for instant authentication
- **Real-time Authentication** - Backend tracks all RFID login attempts
- **Admin Monitoring** - Complete RFID login dashboard
- **Professional UI/UX** - Modern, interactive frontend design
- **Secure Authentication** - JWT-based token system with role-based access

---

## 🔧 Hardware Requirements

### For the RFID Kiosk at Demo Garage:

1. **ESP-32 Development Board**
   - Microcontroller with WiFi capability
   - ~$5-10 per unit

2. **RC522 RFID Reader Module**
   - 13.56 MHz RFID reader
   - Includes RFID cards/tags
   - ~$5-8 per unit

3. **16x2 LCD 1602 Display (I2C)**
   - For user feedback
   - ~$3-5 per unit

4. **Status LEDs**
   - Green LED (authentication success)
   - Red LED (authentication failed)
   - ~$0.50 per unit

5. **Buzzer/Speaker**
   - 5V passive buzzer for audio feedback
   - ~$1-2 per unit

6. **Power Supply**
   - 5V USB power adapter
   - ~$5-10

### Pin Configuration (from esp32_rfid_controller.ino):
```
- RST_PIN = GPIO 22 (RC522 Reset)
- SS_PIN = GPIO 21 (RC522 Chip Select)
- GREEN_LED = GPIO 32
- RED_LED = GPIO 33
- BUZZER_PIN = GPIO 25
```

### Wiring Diagram:
```
ESP-32    -->  RC522 Reader
GPIO 22   -->  RST
GPIO 21   -->  SS (CS)
GPIO 23   -->  MOSI
GPIO 19   -->  MISO
GPIO 18   -->  SCK

ESP-32    -->  LCD (I2C 0x27)
GPIO 21   -->  SDA (via voltage divider)
GPIO 22   -->  SCL (via voltage divider)

ESP-32    -->  Status LED & Buzzer
GPIO 32   -->  Green LED (through 330Ω resistor)
GPIO 33   -->  Red LED (through 330Ω resistor)
GPIO 25   -->  Buzzer
```

---

## 🚀 Software Setup

### 1. Backend Setup (Node.js + Express)

#### Install Dependencies:
```bash
cd c:\bike-rental
npm install express mongoose bcryptjs jsonwebtoken cors dotenv
```

#### Environment Variables (.env):
```env
MONGODB_URI=mongodb+srv://username:password@cluster0.mongodb.net/velotrack?retryWrites=true&w=majority
JWT_SECRET=your_super_secret_jwt_key_change_this
ADMIN_EMAIL=admin@velotrack.com
ADMIN_PASSWORD=Admin@1234
SETUP_SECRET=your_setup_secret_key
PORT=5000
```

#### Start the Server:
```bash
node server.js
```

Expected Output:
```
✅ Connected to MongoDB Atlas (Cluster0)
✅ Default admin created → admin@velotrack.com / Admin@1234
🚲 VeloTrack backend running on http://localhost:5000
```

---

### 2. Frontend Setup

#### Option A: Local Files (Simple)
1. Copy these files to your `Public/` folder:
   - `index_new.html` → `index.html` (or keep both)
   - `login_new.html` → `login.html` (or keep both)
   - `admin-dashboard_new.html` → `admin-dashboard.html`
   - `dashboard_new.html` → `dashboard.html`

2. Access via:
   - Home: `http://localhost:5000/index.html`
   - Login: `http://localhost:5000/login.html`
   - Admin: `http://localhost:5000/admin-dashboard.html`
   - User Dashboard: `http://localhost:5000/dashboard.html`

#### Option B: Separate Frontend Server (Recommended for Production)
```bash
# If using separate frontend server, update API URLs in the HTML files
# Change all fetch() calls to point to your backend IP:
# From: /api/auth/login
# To:   http://YOUR_BACKEND_IP:5000/api/auth/login
```

---

### 3. ESP-32 Arduino Setup

#### Step 1: Install Arduino IDE
- Download from: https://www.arduino.cc/en/software

#### Step 2: Add ESP-32 Board Support
1. Go to **File > Preferences**
2. Add this to "Additional Boards Manager URLs":
   ```
   https://dl.espressif.com/dl/package_esp32_index.json
   ```
3. Go to **Tools > Board > Boards Manager**
4. Search and install "esp32 by Espressif Systems"

#### Step 3: Install Required Libraries
1. **Sketch > Include Library > Manage Libraries**
2. Install:
   - `MFRC522` (by GithubCommunity)
   - `LiquidCrystal_I2C` (by Frank de Brabander)
   - `ArduinoJson` (by Benoit Blanchon) - Optional but recommended

#### Step 4: Load the Code
1. Copy entire `esp32_rfid_controller.ino` code
2. Paste into Arduino IDE
3. Update configuration at the top:
   ```cpp
   #define WIFI_SSID "YOUR_WIFI_SSID"
   #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
   #define API_BASE_URL "http://your-backend-ip:5000"
   ```

#### Step 5: Select Board & Port
1. **Tools > Board > ESP32 Dev Module**
2. **Tools > Port > COM[X]** (where your ESP-32 is connected)
3. **Tools > Upload Speed > 115200**

#### Step 6: Upload
1. Click **Upload** button (→ icon)
2. Wait for "Leaving... Hard resetting via RTS pin..."
3. Open **Serial Monitor** (Tools > Serial Monitor) to see debug output

---

## 📱 System Architecture

### User Flow - RFID Authentication:

```
1. User taps RFID card at Station Kiosk
   ↓
2. ESP-32 reads card UID
   ↓
3. ESP-32 sends to Backend: GET /api/auth/user-by-rfid/:uid
   ↓
4. Backend validates card & logs attempt in RfidLogin collection
   ↓
5a. IF Valid:
    - Backend returns user data
    - ESP-32 shows ✅ Success (Green LED + Beep)
    - Backend starts ride
    - User can select bike on website
   
5b. IF Invalid:
    - Backend returns 404
    - ESP-32 shows ❌ Failed (Red LED + Multiple Beeps)
    - Admin sees failed attempt in dashboard
   ↓
6. User completes ride (taps card again)
   ↓
7. ESP-32 sends to Backend: POST /api/rides/end
   ↓
8. Backend ends ride, calculates fare, deducts from wallet
   ↓
9. Admin can see all RFID logins in monitoring section
```

---

## 🔑 API Endpoints

### Authentication
- `POST /api/auth/register` - User registration
- `POST /api/auth/login` - User login
- `GET /api/auth/me` - Get current user (protected)
- `GET /api/auth/user-by-rfid/:uid` - Verify RFID card (ESP-32 calls this)

### RFID & Admin
- `GET /api/admin/rfid-logins` - All RFID login history
- `GET /api/admin/rfid-logins/today` - Today's RFID stats
- `GET /api/admin/rfid-logins/user/:userId` - User's RFID history
- `GET /api/admin/rfid-stats` - Overall RFID statistics

### Rides
- `POST /api/rides/start` - Start a ride
- `POST /api/rides/end` - End a ride
- `GET /api/rides` - All rides (admin only)
- `GET /api/rides/user/:userId` - User's ride history

---

## 📊 Admin Monitoring Dashboard

### Features:
✅ **Real-time Statistics**
- Total RFID logins
- Successful vs. failed logins
- Success rate percentage
- Daily/weekly trends

✅ **RFID Login Monitoring**
- Complete login history with timestamps
- Filter by status (Success/Failed/Error)
- User information for each login
- IP address tracking

✅ **User Management**
- View all registered users
- RFID card assignment status
- Subscription plans
- Account activity

✅ **Ride Analytics**
- All active and completed rides
- Fare tracking
- Duration and distance metrics

---

## 🛡️ Security Features

1. **JWT Authentication**
   - 7-day token expiration
   - Role-based access control (User/Admin)

2. **Password Security**
   - Bcryptjs hashing (12 rounds)
   - Minimum 6 characters

3. **RFID Card Validation**
   - Duplicate card prevention
   - Active user verification
   - Unauthorized access logging

4. **API Protection**
   - Auth middleware on protected routes
   - CORS enabled for cross-origin requests
   - Rate limiting recommended for production

---

## 🔄 Workflow Examples

### Example 1: New User RFID Registration

```
1. Admin creates user via registration:
   - Name: John Doe
   - Email: john@example.com
   - Password: secure123

2. Admin assigns RFID card:
   PATCH /api/auth/rfid
   {
     "userId": "user_id_here",
     "rfidUid": "A1B2C3D4"
   }

3. User taps card at station:
   GET /api/auth/user-by-rfid/A1B2C3D4
   ✅ Authentication successful!
   → User can now select and rent bikes

4. Admin sees login on dashboard:
   - Dashboard shows: 1 successful login
   - RFID Logs show: John Doe - ✅ Success - Timestamp
```

### Example 2: Failed RFID Authentication

```
1. Unregistered/Invalid card tapped:
   GET /api/auth/user-by-rfid/INVALID123

2. Backend response:
   404 Not Found - Card not recognized

3. Hardware feedback:
   - ESP-32 Red LED turns on
   - 3 short beeps
   - LCD shows: "Auth Failed! Card not valid"

4. Admin monitoring:
   - Dashboard updates: 1 failed login
   - RFID Logs: INVALID123 - ❌ Invalid - IP Address
```

### Example 3: Complete Ride Cycle

```
1. User taps card to START:
   - ESP-32 reads card
   - Backend verifies user and calls POST /api/rides/start
   - Ride status: "active"
   - Green LED + Success beep

2. User rides bike for 15 minutes

3. User taps card to END:
   - ESP-32 reads card
   - Backend calls POST /api/rides/end
   - Fare calculated: ৳30 (15 min × ৳2/min)
   - Wallet deducted: ৳(previous - 30)
   - Ride status: "completed"

4. Admin verification:
   - Sees ride in "All Rides" section
   - Duration: 00:15:32
   - Fare: ৳30
   - Distance: 2.5 km
   - User: John Doe
```

---

## 🧪 Testing

### Test RFID Card Responses:

1. **Successful Authentication**
   ```bash
   curl http://localhost:5000/api/auth/user-by-rfid/VALIDCARDUID
   # Response: 200 OK with user data
   ```

2. **Invalid Card**
   ```bash
   curl http://localhost:5000/api/auth/user-by-rfid/INVALIDCARD
   # Response: 404 Not Found
   ```

3. **Check Admin Stats**
   ```bash
   curl -H "Authorization: Bearer YOUR_ADMIN_TOKEN" \
        http://localhost:5000/api/admin/rfid-stats
   # Response: Statistics object
   ```

---

## 📱 Mobile/Browser Compatibility

✅ **Tested & Working On:**
- Chrome/Edge (Desktop & Mobile)
- Firefox
- Safari
- Mobile browsers (iOS, Android)

✅ **Responsive Design:**
- 1920px+ (Desktop)
- 1024px (Tablet landscape)
- 768px (Tablet portrait)
- 480px (Mobile)

---

## ⚠️ Troubleshooting

### ESP-32 Not Connecting to WiFi:
1. Check SSID and password in code
2. Ensure ESP-32 is in range of WiFi router
3. View Serial Monitor output for error messages
4. Try rebooting ESP-32

### RFID Reader Not Recognized:
1. Check wiring (RST=22, SS=21)
2. Verify SPI library is loaded
3. Try resetting the board
4. Check RC522 voltage (should be 3.3V)

### Backend Connection Failed:
1. Verify MongoDB URI is correct
2. Check whitelist IP in MongoDB Atlas
3. Ensure all environment variables are set
4. Check MongoDB cluster is active

### Login Not Working:
1. Verify user account exists
2. Check password is correct
3. Look for errors in browser console
4. Verify backend is running

---

## 📈 Future Enhancements

- [ ] Add SMS/Email notifications
- [ ] Implement bike geofencing
- [ ] Add payment gateway integration
- [ ] Real-time bike location tracking
- [ ] QR code alternative authentication
- [ ] Bike maintenance alerts
- [ ] User subscription management
- [ ] Analytics dashboard with charts

---

## 📞 Support & Contact

- **Project**: VeloTrack RFID Bike Rental
- **Version**: 1.0.0
- **Last Updated**: 2026

For issues or questions, refer to the code documentation in:
- `server.js` - Backend API documentation
- `esp32_rfid_controller.ino` - Hardware firmware documentation
- Frontend HTML files - Inline CSS and JavaScript comments

---

**Happy Biking! 🚲**
