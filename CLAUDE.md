# Laarif StrikeSense: rules that always apply

## The product
- StrikeSense is for **surfcasting only**. No boat, kayak, lure or other fishing styles, anywhere (app, profile, rankings, competitions).
- International: anyone, in any country, can use the app.

## Working with the owner
- Simple English, tables.
- A mockup before any visual feature.
- Confirm understanding before big changes.
- Always deliver downloadable files.
- Priorities: alarms > buttons > live data > status > background. Everything must stay smooth.

## Accounts (Firebase project strikesense-acdcf, Firestore eur3)
- Sign up needs **both** a verified phone number (SMS code) and a verified email (link).
- Log in with email + password, or phone + SMS code (same account).
- Country is set automatically from the phone's location. City and phone number are required.
- After confirmation the user **cannot change the country or the email**; only the admin can (the user contacts the admin).
- Registration date, time and location are saved automatically.
- Required: first name, last name, country (automatic), city, date of birth (13+), sex, phone, email.
- Optional: nickname (shown to others when set), profile photo (default male/female avatar), fishing licence.
- Never shown to other users: phone, email, date of birth, exact location.

## Safety
- Report on every profile, chat and message. End chat and Block at any time.
- The admin sees all reports and can restrict an account: hide it from the map, block it from chat, or ban it.

## Online map: visible or hidden
- On going online the app shows how many surfcasters are around (about 20 km), names hidden.
- Choices: Visible approximate (300-500 m circle), Visible exact, Stay hidden.
- Only visible users see each other and can chat. Hidden users see only the number.
- Approximate users see everyone as circles. Exact spots are shared only between users who both chose exact.
- Visible stays on for at least 50 minutes. Exact -> approximate is allowed any time. Hiding early only with "I feel unsafe: report + hide now".
- Auto-offline: rod off 30 min, or after 12 h.
