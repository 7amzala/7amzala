#!/usr/bin/env python3
"""NOT APPLIED - kept for later. On the real rod the watch got almost no
answers with this ("connected - waiting for data"), so the sketches use
the send-to-all method again (see git history, 24fcced).

Rod Bluetooth: each answer only to the phone/watch that asked.

Before, every answer went to both connections (all notifications go to
every subscriber), in pieces as small as the smaller MTU of the two. With
the watch asking 10x a second, the phone also received all of that and
waited behind it.

Now (NimBLE, the ESP32-S3 / XIAO): the connection that wrote a request is
remembered with it, the answer is sent to that connection only, in pieces
of that connection's own size. A piece the Bluetooth stack cannot take yet
is tried again on the next loop (nothing is lost). Without NimBLE (older
ESP32 boards) it works as before.

Run after add_watch_pages.py.

    python3 add_ble_per_client.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


# ---------- low-level helpers (real NimBLE; the PC test brings its own) ----------
rep('''static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;''', '''static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;

#if defined(CONFIG_NIMBLE_ENABLED) && !defined(BLE_MOCK)
#include "host/ble_hs.h"
#endif''')

# inside the class: plain functions at the top of an .ino make the Arduino
# IDE put its function list there, before Vec3 is known
rep('''class BleServer
{
public:
''', '''class BleServer
{
public:
#if defined(CONFIG_NIMBLE_ENABLED) && !defined(BLE_MOCK)
  // (inside the class: plain functions up here would make the Arduino IDE
  // put its function list before Vec3 is known)

  // One piece to one connection. 0 = sent, else the Bluetooth stack's error code
  static int bleNotifyTo(BLECharacteristic* c, uint16_t conn, const uint8_t* data, size_t length)
  {
    os_mbuf* om = ble_hs_mbuf_from_flat(data, length);

    if(om == nullptr)
      return -1;                                                  // no memory right now

    return ble_gatts_notify_custom(conn, c->getHandle(), om);     // om is always used up
  }

  static size_t bleMtu(uint16_t conn)
  {
    return ble_att_mtu(conn);
  }

  static bool bleConnected(uint16_t conn)
  {
    ble_gap_conn_desc d;
    return ble_gap_conn_find(conn, &d) == 0;
  }
#endif

''')

# ---------- remember who wrote each line ----------
rep('''    void onWrite(BLECharacteristic* c) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i]);
    }''', '''    void onWrite(BLECharacteristic* c) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i], NO_CONN);
    }

#if defined(CONFIG_NIMBLE_ENABLED)
    // NimBLE also says which connection wrote it: answer only that one
    void onWrite(BLECharacteristic* c, ble_gap_conn_desc* desc) override
    {
      auto v = c->getValue();

      for(size_t i = 0; i < v.length(); i++)
        owner->received(v[i], desc != nullptr ? desc->conn_handle : NO_CONN);
    }
#endif''')

rep('''  void received(char ch)
  {
    portENTER_CRITICAL(&bleLock);

    if(ch == '\\n')
    {
      int next = (qHead + 1) % REQ_QUEUE;

      if(next != qTail)
      {
        memcpy(queue[qHead], partial, partialLen);
        queue[qHead][partialLen] = 0;
        qHead = next;
      }

      partialLen = 0;
    }''', '''  void received(char ch, uint16_t conn)
  {
    portENTER_CRITICAL(&bleLock);

    // a line is written by one phone/watch: another one starting = new line
    if(partialLen > 0 && conn != partialConn)
      partialLen = 0;

    partialConn = conn;

    if(ch == '\\n')
    {
      int next = (qHead + 1) % REQ_QUEUE;

      if(next != qTail)
      {
        memcpy(queue[qHead], partial, partialLen);
        queue[qHead][partialLen] = 0;
        queueConn[qHead] = conn;
        qHead = next;
      }

      partialLen = 0;
    }''')

rep('''  bool popLine(char* line)
  {
    bool got = false;

    portENTER_CRITICAL(&bleLock);

    if(qTail != qHead)
    {
      memcpy(line, queue[qTail], REQ_MAX);''', '''  bool popLine(char* line, uint16_t* conn)
  {
    bool got = false;

    portENTER_CRITICAL(&bleLock);

    if(qTail != qHead)
    {
      memcpy(line, queue[qTail], REQ_MAX);
      *conn = queueConn[qTail];''')

rep('''    if(!popLine(line))
      return;

    dispatch(line);''', '''    uint16_t conn = NO_CONN;

    if(!popLine(line, &conn))
      return;

    replyConn = conn;
    replyTries = 0;
    dispatch(line);''')

rep('''  char partial[REQ_MAX];
  int partialLen = 0;''', '''  char partial[REQ_MAX];
  int partialLen = 0;
  uint16_t queueConn[REQ_QUEUE];
  uint16_t partialConn = 0xFFFF;

  // who the answer being sent is for
  static const uint16_t NO_CONN = 0xFFFF;
  uint16_t replyConn = 0xFFFF;
  int replyTries = 0;
  bool directOff = false;                // sending to one device failed: send to all''')

# ---------- send each piece to the asker only ----------
rep('''      if(clients > 0 && micros() - lastPiece >= PIECE_GAP_US)
      {
        size_t n = out.length() - outPos;

        if(n > chunk)
          n = chunk;

        tx->setValue((uint8_t*)out.c_str() + outPos, n);
        tx->notify();

        outPos += n;
        lastPiece = micros();
      }''', '''      if(clients > 0 && micros() - lastPiece >= PIECE_GAP_US)
      {
        size_t n = out.length() - outPos;
        lastPiece = micros();

#if defined(CONFIG_NIMBLE_ENABLED)
        if(replyConn != NO_CONN && !directOff)
        {
          // only the phone/watch that asked, in pieces of its own size
          size_t mtu = bleMtu(replyConn);
          size_t piece = mtu > 23 ? mtu - 3 : 20;

          if(piece > 512)
            piece = 512;

          if(n > piece)
            n = piece;

          int rc = 0;

          if(!bleConnected(replyConn))
          {
            outPos = out.length();               // it has gone: drop the answer
          }
          else if((rc = bleNotifyTo(tx, replyConn, (const uint8_t*)out.c_str() + outPos, n)) == 0)
          {
            outPos += n;
            replyTries = 0;
          }
          else if(++replyTries >= 3)
          {
            // sending to one device does not work here: back to sending to all
            directOff = true;
            replyTries = 0;
            Serial.printf("Bluetooth: send to one device failed (error %d) - sending to all instead\\n", rc);
          }
        }
        else
#endif
        {
          if(n > chunk)
            n = chunk;

          tx->setValue((uint8_t*)out.c_str() + outPos, n);
          tx->notify();

          outPos += n;
        }
      }''')

rep('''  bool connected() const
  {
    return clients > 0;
  }''', '''  bool connected() const
  {
    return clients > 0;
  }

  // true after sending to one device failed (then everything goes to all)
  bool sendsToAll() const
  {
    return directOff;
  }''')

open(path, 'w').write(s)
print("ok", path)
