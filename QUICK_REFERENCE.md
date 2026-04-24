# VeloTrack RFID System - Quick Reference

## 🔧 Configuration Checklist

### Before Running:

- [ ] Install Node.js dependencies: `npm install`
- [ ] Update `.env` file with MongoDB URI
- [ ] Update ESP-32 WiFi credentials in `esp32_rfid_controller.ino`
- [ ] Update API_BASE_URL in ESP-32 code
- [ ] Install Arduino IDE and ESP-32 board support
- [ ] Connect ESP-32 via USB

### Default Admin Credentials (change after first login!)
```
Email: admin@velotrack.com
Password: Admin@1234
```

## 🚀 Quick Start Commands

```bash
# 1. Start Backend
node server.js

# 2. Open Frontend
# Visit: http://localhost:5000/index.html

# 3. Register New User
# Go to: http://localhost:5000/register.html

# 4. Login as Admin
# Email: admin@velotrack.com
# Go to: http://localhost:5000/admin-dashboard.html

# 5. View RFID Logs
# Admin Dashboard → RFID Logins tab
```

## 📡 ESP-32 Configuration

### WiFi & API Settings (in esp32_rfid_controller.ino):
```cpp
#define WIFI_SSID "Your_WiFi_Name"
#define WIFI_PASSWORD "Your_Password"
#define API_BASE_URL "http://192.168.1.100:5000"  // Your backend IP
#define BIKE_ID "BIKE-001"
#define STATION_ID "STATION-001"
```

### Serial Monitor Output Expected:
```
=== VeloTrack RFID Controller Starting ===
✅ RFID Reader initialized
🔌 Connecting to WiFi: Your_WiFi_Name
✅ WiFi connected!
📡 IP: 192.168.1.50
🚀 All systems ready! Waiting for RFID card...

[When card is tapped]
📷 RFID Card Detected: A1B2C3D4E5F6
🔐 Authenticating RFID card with backend...
✅ RFID Authentication Successful!
🚴 Starting ride...
```

## 📊 Database Collections

### MongoDB Collections Created:
1. **users** - User profiles with RFID assignment
2. **rides** - Ride records (start, end, fare, etc.)
3. **rfidlogins** - Complete RFID authentication logs

### Sample User Document:
```json
{
  "firstName": "John",
  "lastName": "Doe",
  "email": "john@example.com",
  "phone": "+8801234567890",
  "city": "Dhaka",
  "password": "$2a$12$...",
  "plan": "basic",
  "rfidUid": "A1B2C3D4",
  "walletBalance": 500,
  "isActive": true,
  "role": "user",
  "createdAt": "2026-04-17T10:00:00.000Z"
}
```

## 🎯 Testing Scenarios

### Scenario 1: Valid RFID Card
```
1. Tap registered card at kiosk
2. ESP-32 green LED turns on
3. 2 beeps sound
4. User authenticated
5. User can select bike on website
6. Admin sees success in dashboard
```

### Scenario 2: Invalid RFID Card
```
1. Tap unregistered/invalid card
2. ESP-32 red LED turns on
3. 3 short beeps sound
4. "Card not valid" on LCD display
5. Admin sees failed attempt in logs
```

### Scenario 3: Server Connection Error
```
1. ESP-32 can't reach backend
2. Red LED + 2 beeps pattern
3. "Server error" on LCD
4. Attempt logged as "error" status
```

## 🖥️ Admin Dashboard Navigation

### Main Sections:
1. **Dashboard**
   - Overall statistics
   - Success rate
   - Today's trends
   - Active users

2. **RFID Logins**
   - Live login history
   - Filter by status
   - User information
   - IP tracking

3. **All Rides**
   - Current and past rides
   - User details
   - Fare information
   - Ride duration

4. **Users**
   - All registered users
   - RFID assignment status
   - Payment plans
   - Account status

## 😕 Common Issues & Fixes

| Issue | Cause | Solution |
|-------|-------|----------|
| ESP-32 won't upload | Wrong COM port | Check Tools > Port |
| WiFi not connecting | Wrong SSID/password | Verify credentials in code |
| RFID reader not found | SPI pins incorrect | Check pin definitions |
| Backend connection failed | Wrong IP address | Use `ipconfig` to find IP |
| Login fails | Wrong credentials | Use admin@velotrack.com |
| RFID logs empty | Collections not created | Backend creates on first use |

## 🔐 Default Ports & URLs

```
Backend API:      http://localhost:5000
Frontend:         http://localhost:5000/index.html
Admin Dashboard:  http://localhost:5000/admin-dashboard.html
Login Page:       http://localhost:5000/login.html
Register Page:    http://localhost:5000/register.html
```

## 📞 API Response Examples

### Successful RFID Verification:
```json
{
  "authorized": true,
  "user": {
    "id": "507f1f77bcf86cd799439011",
    "firstName": "John",
    "lastName": "Doe",
    "plan": "basic",
    "walletBalance": 500
  }
}
```

### Failed RFID Verification:
```json
{
  "authorized": false,
  "message": "RFID card not recognized."
}
```

### RFID Stats Response:
```json
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

---

**Need Help?** Check the full documentation in `SETUP_AND_INTEGRATION_GUIDE.md`
