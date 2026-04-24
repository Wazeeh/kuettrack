# 🎯 VeloTrack Architecture & Workflows - Visual Guide

## System Overview Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    USERS & ADMINS                           │
└─────────────────────────────────────────────────────────────┘
    ↑              ↑                    ↑                  ↑
    │              │                    │                  │
┌───────────┐ ┌────────────┐    ┌──────────────┐  ┌──────────────┐
│  Browser  │ │  Mobile    │    │  Admin       │  │  User        │
│  (Web)    │ │  Browser   │    │  Dashboard  │  │  Dashboard  │
└───────────┘ └────────────┘    └──────────────┘  └──────────────┘
    ↓              ↓                    ↓                  ↓
    └──────────────┼────────────────────┼──────────────────┘
                   ↓
    ┌──────────────────────────────────────────────┐
    │       Backend API (Node.js/Express)         │
    │  ├─ Authentication & JWT                    │
    │  ├─ RFID Login Tracking                     │
    │  ├─ Ride Management                         │
    │  ├─ User Management                         │
    │  └─ Admin Monitoring                        │
    └──────────────────────────────────────────────┘
           ↑                              ↑
           │                              │
    ┌──────────────┐              ┌──────────────┐
    │  MongoDB     │              │  ESP-32      │
    │  Atlas       │              │  Hardware    │
    │  Database    │              │  (RFID)      │
    └──────────────┘              └──────────────┘
                                         ↓
                                  ┌──────────────┐
                                  │ RFID Kiosk   │
                                  │ at Station   │
                                  └──────────────┘
```

---

## RFID Authentication Flow

```
┌─────────────────────────────────────────────────────────────┐
│ STEP 1: User Taps RFID Card at Station                     │
└─────────────────────────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────────────────────────┐
│ STEP 2: ESP-32 Reads Card UID (e.g., A1B2C3D4)            │
│         - Displays "Scanning..." on LCD                     │
│         - Sends beep notification                           │
└─────────────────────────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────────────────────────┐
│ STEP 3: ESP-32 → Backend: GET /api/auth/user-by-rfid/:uid │
│         - Sends card UID to backend for validation         │
│         - Waits for response (2-3 seconds)                 │
└─────────────────────────────────────────────────────────────┘
           ↓
    ┌─────────────────────────────────────┐
    │  STEP 4: Backend Validation         │
    │  ├─ Find user by RFID UID          │
    │  ├─ Check if user is active        │
    │  └─ Log attempt in RfidLogins DB   │
    └─────────────────────────────────────┘
           ↓             ↓
        SUCCESS      FAILURE
           ↓             ↓
    ┌───────────┐   ┌────────────┐
    │ HTTP 200  │   │ HTTP 404   │
    │ User data │   │ Card not   │
    │ + JWT     │   │ recognized │
    └───────────┘   └────────────┘
           ↓             ↓
    ┌───────────┐   ┌────────────┐
    │GREEN LED  │   │ RED LED    │
    │ ✓ ✓ ✓    │   │ ✗ ✗ ✗      │
    │(2 beeps)  │   │ (3 beeps)  │
    └───────────┘   └────────────┘
           ↓             ↓
    ┌───────────────────────────┐
    │ LCD Display Status         │
    │                            │
    │ SUCCESS:                   │
    │ "Auth Success!"            │
    │ "Select a Bike"            │
    │                            │
    │ FAILED:                    │
    │ "Auth Failed!"             │
    │ "Card not valid"           │
    └───────────────────────────┘
```

---

## Complete Ride Lifecycle

```
1. USER TAPS CARD TO START RIDE
   ├─ ESP-32 reads RFID
   ├─ Backend verifies user
   ├─ Backend calls POST /api/rides/start
   └─ Ride status: "active"
                    ↓

2. BACKEND CREATES RIDE RECORD
   ├─ userId: Store user ID
   ├─ rfidUid: Store card UID
   ├─ bikeId: Store bike ID
   ├─ stationStart: Store start location
   ├─ startTime: Record start timestamp
   └─ status: Set to "active"
                    ↓

3. USER SELECTS BIKE FROM WEBSITE
   ├─ Dashboard shows available bikes
   ├─ User clicks "Start Ride"
   ├─ Ride becomes active
   └─ Timer starts on user dashboard
                    ↓

4. USER RIDES BICYCLE
   ├─ Location tracking (optional)
   ├─ Distance calculation
   ├─ Real-time stats on user dashboard
   └─ Ride continues until completion
                    ↓

5. USER TAPS CARD TO END RIDE
   ├─ ESP-32 reads same/different RFID
   ├─ Sends POST /api/rides/end
   └─ Includes rideId and end stationId
                    ↓

6. BACKEND CALCULATES FARE
   ├─ Calculate duration:
   │  └─ endTime - startTime = elapsed seconds
   ├─ Convert to minutes: 932 seconds = 15.5 minutes
   ├─ Calculate fare: 15.5 min × ₳2/min = ₳31
   ├─ Estimate distance: 15.5 min × 0.167 km/min = 2.5 km
   └─ Minimum fare: ₳2 (even for short rides)
                    ↓

7. BACKEND UPDATES USER & RIDE
   ├─ Find user by RFID
   ├─ Deduct fare from wallet:
   │  └─ newBalance = walletBalance - fare
   ├─ Update ride record:
   │  ├─ endTime: Current timestamp
   │  ├─ stationEnd: Return station
   │  ├─ duration: "00:15:32"
   │  ├─ distanceKm: 2.5
   │  ├─ fare: 31
   │  └─ status: "completed"
   └─ Log transaction
                    ↓

8. HARDWARE FEEDBACK
   ├─ If successful:
   │  ├─ Green LED ON
   │  ├─ 3 beeps (success pattern)
   │  └─ LCD: "Ride Completed! Thank You"
   └─ If error:
      ├─ Red LED ON
      ├─ 2 beeps (error pattern)
      └─ LCD: "Error ending ride"
                    ↓

9. USER SEES UPDATE
   ├─ Dashboard refreshes
   ├─ Shows completed ride:
   │  ├─ Duration: 00:15:32
   │  ├─ Fare: ₳31
   │  ├─ Distance: 2.5 km
   │  └─ Status: Completed ✓
   ├─ Wallet balance updated:
   │  └─ Previous: ₳1000 → New: ₳969
   └─ Ride added to history
                    ↓

10. ADMIN SEES IN DASHBOARD
    ├─ New ride appears in "All Rides"
    ├─ User statistics updated:
    │  ├─ Total rides: +1
    │  ├─ Total distance: +2.5 km
    │  └─ Total spent: +₳31
    ├─ Revenue tracking:
    │  └─ Daily revenue: +₳31
    └─ Analytics updated
```

---

## Admin Monitoring Dashboard

```
┌─────────────────────────────────────────────────────────────┐
│ ADMIN DASHBOARD                                             │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│ ┌──────────────────┐  ┌──────────────────┐                 │
│ │ Total RFID Logins│  │ Successful Logins│                 │
│ │      245         │  │      234         │                 │
│ └──────────────────┘  └──────────────────┘                 │
│                                                              │
│ ┌──────────────────┐  ┌──────────────────┐                 │
│ │ Failed Logins    │  │ Success Rate     │                 │
│ │       8          │  │     95.51%       │                 │
│ └──────────────────┘  └──────────────────┘                 │
│                                                              │
├─────────────────────────────────────────────────────────────┤
│ RFID LOGIN HISTORY (Last 10)                               │
├──────────┬─────────────────┬──────────┬──────────┬────────┤
│ Time     │ User            │ Card UID │ Status   │ IP     │
├──────────┼─────────────────┼──────────┼──────────┼────────┤
│ 14:32:15 │ John Doe        │ A1B2C3D4 │ ✅ OK    │ 192... │
│ 14:15:42 │ Jane Smith      │ X9Y8Z7W6 │ ✅ OK    │ 192... │
│ 14:02:33 │ Unknown         │ INVALID1 │ ❌ FAIL  │ 192... │
│ 13:45:20 │ Bob Johnson     │ M4N5O6P7 │ ✅ OK    │ 192... │
│ 13:22:11 │ [System Error]  │ NONE     │ ⚠️ ERROR │ 192... │
└──────────┴─────────────────┴──────────┴──────────┴────────┘
│                                                              │
│ Filters: [All] [Success] [Failed] [Error] | Refresh       │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## Database Schema Visualization

```
USERS Collection
├─ _id
├─ firstName
├─ lastName
├─ email (unique)
├─ phone
├─ city
├─ password (hashed)
├─ plan (basic/monthly/student)
├─ rfidUid ← LINKED TO CARD
├─ walletBalance
├─ isActive
├─ role (user/admin)
└─ createdAt

      ↓ references
      
RFIDLOGINS Collection
├─ _id
├─ userId → User._id
├─ rfidUid ← Card UID
├─ status (success/invalid_card/error)
├─ stationId
├─ ipAddress
├─ timestamp
└─ deviceInfo

      ↓ references
      
RIDES Collection
├─ _id
├─ userId → User._id
├─ rfidUid ← Card UID
├─ bikeId
├─ stationStart
├─ stationEnd
├─ startTime
├─ endTime
├─ duration (HH:MM:SS)
├─ distanceKm
├─ fare
└─ status (active/completed/error)
```

---

## Hardware Pin Connections

```
        ┌─────────────────────────┐
        │      ESP-32 Board       │
        │                         │
    ┌───┤ GPIO 22 (RST)    ───┬──┤
    │   │ GPIO 21 (SS/CS)   │  │
    │   │ GPIO 23 (MOSI)    │  │
    │   │ GPIO 19 (MISO)    │  │
    │   │ GPIO 18 (SCK)     │  │
    │   │ GPIO 21 (SDA)     │  │
    │   │ GPIO 22 (SCL)     │  │
    │   │ GPIO 32 (LED G)   │  │
    │   │ GPIO 33 (LED R)   │  │
    │   │ GPIO 25 (Buzzer)  │  │
    │   └─────────────────────┘  │
    │                            │
    └────────────────────────────┘

    ┌──────────────┐    ┌─────────┐
    │  RC522 RFID  │    │   LCD   │
    │              │    │         │
    │ RST ←→ GP22  │    │ SDA ←→  │
    │ SS  ←→ GP21  │    │ SCL ←→  │
    │ MOSI←→ GP23  │    └─────────┘
    │ MISO←→ GP19  │
    │ SCK ←→ GP18  │         ┌──────────┐
    └──────────────┘         │ Feedback │
                             │          │
    ┌──────────┐  ┌───────┐  │ LED-G ← │ GP32
    │   Buzzer │  │ LED-R │  │ LED-R ← │ GP33
    │   ← GP25 │  │ ← GP33│  │ Buzz ← │ GP25
    └──────────┘  └───────┘  └──────────┘
```

---

## Authentication Flow Sequence

```
User         ESP-32          Backend         Database
 │              │                │               │
 │──Tap Card───→│                │               │
 │              │                │               │
 │              │──Read UID──────→│               │
 │              │                │               │
 │              │                │──Find User──→│
 │              │                │               │
 │              │                │←─User Data──│
 │              │                │               │
 │              │                │──Log Entry──→│
 │              │                │               │
 │              │←─Response(200)─│               │
 │              │   + User data  │               │
 │              │                │               │
 │              │═══Green LED════→ Display Success
 │              │═══2 Beeps══════→
 │              │═══Update LCD───→
 │              │                │
 │←─Auth Done───│                │
 │              │
 │─Select Bike──────────────────→│               │
 │              │                │──Create Ride→│
 │              │                │               │
 │              │                │←─Ride ID────│
 │              │
 │←─Ride Active─|
 │              │
 │   (Riding...)│
 │              │
 │──Tap Again───→│                │               │
 │              │                │               │
 │              │──End Ride─────→│               │
 │              │   + UID,RideID │               │
 │              │                │               │
 │              │                │─Calculate───→│
 │              │                │  Fare        │
 │              │                │               │
 │              │                │←─Update────│
 │              │                │               │
 │              │←─Response(200)─│               │
 │              │  Duration,Fare │               │
 │              │                │               │
 │              │═══Green LED════→ Display Done
 │              │═══3 Beeps══────→
 │
 │←─Ride Complete
 │
```

---

## Admin Access & Monitoring

```
Admin Login
    ↓
┌─────────────────────────────────┐
│  Admin Dashboard                │
├─────────────────────────────────┤
│ Dashboard Tab                   │
│ ├─ Total Statistics             │
│ ├─ Success Rate                 │
│ ├─ Today's Metrics              │
│ └─ Trends                       │
│                                 │
│ RFID Logins Tab                 │
│ ├─ Complete History             │
│ ├─ Filter by Status             │
│ ├─ User Information             │
│ └─ IP Address Tracking          │
│                                 │
│ All Rides Tab                   │
│ ├─ Current Rides                │
│ ├─ Completed Rides              │
│ ├─ Fare Tracking                │
│ └─ Duration Stats               │
│                                 │
│ Users Tab                       │
│ ├─ All Users List               │
│ ├─ RFID Assignment Status       │
│ ├─ Plan Information             │
│ └─ Account Status               │
│                                 │
│ Settings Tab                    │
│ ├─ System Configuration         │
│ ├─ User Management              │
│ ├─ Reports                      │
│ └─ Maintenance                  │
└─────────────────────────────────┘
```

---

## Error Handling Flow

```
System Error
    ↓
    ├─ Network Error
    │  ├─ ESP-32 offline → Red LED
    │  ├─ Logs "error" status
    │  └─ Admin sees in RFID logs
    │
    ├─ Invalid RFID
    │  ├─ Card not in database
    │  ├─ Backend returns 404
    │  ├─ ESP-32 shows Red LED
    │  └─ Admin can filter by "Failed"
    │
    ├─ User Not Active
    │  ├─ Account suspended
    │  ├─ Card won't authenticate
    │  └─ Admin can see in Users tab
    │
    └─ Wallet Insufficient
       ├─ Ride auto-ends
       ├─ Partial charge applied
       └─ Admin notified
```

---

**VeloTrack Visual Architecture Complete! 🎯**
