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

## Emergency SOS and help requests (opt-in)
- Off until the user turns each one on (Send SOS, Receive SOS, Help requests, optional emergency contact).
- SOS: hold 3 s, choose what happened, 5 s countdown to cancel. Always offer the local emergency number (999/112) first.
- SOS makes the sender visible (exact) and alerts every opted-in surfcaster within 5 km, even hidden or on silent, with name, phone and exact place, only during the SOS.
- Ending: "real" or "mistake". A mistake, or helpers reporting it fake, switches SOS off for that account until the admin turns it on again.
- "I need..." requests: fishing things only (bait, hooks, line, sinkers, power bank, starlights, head torch, knife, rod/reel part, other). 3 km, 2 hours, no selling.
- Helper stars: only the person helped can confirm; 1 star per help; same pair at most 1 star a week. Helper 1+, Trusted helper 10+, Guardian 3+ SOS.
- Emergency contacts: up to 3, chosen by the user; SMS from the user's phone with the place on SOS and again when safe.
- Every visible surfcaster shows their phone battery % (map + profile).
- SOS live board: everyone alerted watches who is going, who arrived, batteries, checks.
- Helpers confirm "I'm going"; a safety check every 15 min (2 min to answer) for the person and each helper. The SOS closes only when the person and every helper confirm safe.
- No answer to a check: alarm to everyone watching, both people's emergency contacts, and all within 5 km, with "call 999/112".
