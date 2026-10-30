set -e
cd $(cd "$(dirname "$0")/.." && pwd)
for sk in StrikeSense/StrikeSense.ino StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino; do git show 3e7b916~1:$sk > $sk; for f in add_ble_lines add_strike_keep5 add_strike_log add_smooth_steady add_instant_switch add_link_colors add_instant_alert add_fight_timer add_part1_fixes add_part2 add_records; do python3 sim/$f.py $sk >/dev/null; done; done
sed -i 's/=== LAARIF STRIKESENSE ROD - version 2.4 (private LoRa: auto channel, crowd mode) ===/=== LAARIF STRIKESENSE ROD - version 3.6 (strike + fight records) ===/' StrikeSense/StrikeSense.ino
sed -i 's/=== LAARIF STRIKESENSE ROD - version 2.2 (Bluetooth: phone + watch, fast watch ring) ===/=== LAARIF STRIKESENSE ROD - version 3.6 (no LoRa; strike + fight records) ===/' StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino
grep -c "version 3.6" StrikeSense/StrikeSense.ino StrikeSense_NoLoRa/StrikeSense_NoLoRa.ino
