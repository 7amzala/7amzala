package com.laarif.strikesense;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelUuid;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

// Bluetooth (BLE) link to the rod. Finds it, connects, reconnects, and
// carries the same requests the web page used (/data, /set?k=..&v=.. ...).
//
//   phone -> rod (write)  : "<id>|<path>\n"
//   rod -> phone (notify) : "#<id> <code> <length>\n<body>", in pieces
@SuppressLint("MissingPermission")
final class RodLink
{
    static final UUID SERVICE = UUID.fromString("6e8a0001-5f2d-4c8e-9a51-2b7c1d3e4f60");
    static final UUID RX = UUID.fromString("6e8a0002-5f2d-4c8e-9a51-2b7c1d3e4f60");
    static final UUID TX = UUID.fromString("6e8a0003-5f2d-4c8e-9a51-2b7c1d3e4f60");
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private static final long SCAN_RESTART_MS = 20000;   // long scans get slowed down by Android
    private static final long CONNECT_TIMEOUT_MS = 15000;
    private static final long RETRY_MS = 1500;

    enum State { NO_PERMISSION, BLUETOOTH_OFF, NOT_PAIRED, SEARCHING, CONNECTING, CONNECTED }
    enum PairState { IDLE, WORKING, OK, FAILED }

    // ---------- pairing: this app talks only to its own rod ----------
    //
    // A rod shows its ID (ROD-4F2A) only while it is open for pairing (the
    // first 3 min after switching on). The user picks it and types its PIN
    // (on the rod box); Android pairs (encrypted) and from then on the app
    // connects only to that rod's address.

    static final class RodSeen
    {
        final String id, address;
        final int rssi;
        final long at;

        RodSeen(String id, String address, int rssi)
        {
            this.id = id;
            this.address = address;
            this.rssi = rssi;
            at = System.currentTimeMillis();
        }
    }

    private volatile String pairedAddress = "";
    private volatile String pairedId = "";
    private volatile boolean pairScan = false;
    private final Map<String, RodSeen> pairSeen = new ConcurrentHashMap<>();
    private volatile PairState pairState = PairState.IDLE;
    private String pairAddress = "", pairIdWanted = "", pairPin = "";
    private boolean receiversOn = false;

    interface Listener
    {
        void onData(String json);
    }

    static final class Response
    {
        final int code;
        final byte[] body;

        Response(int code, byte[] body)
        {
            this.code = code;
            this.body = body;
        }

        String text()
        {
            return new String(body, StandardCharsets.UTF_8);
        }
    }

    private static final class Pending
    {
        final String path;
        final CountDownLatch done = new CountDownLatch(1);
        volatile Response response;

        Pending(String path)
        {
            this.path = path;
        }
    }

    private static RodLink instance;

    private final Context app;
    private final BluetoothAdapter adapter;
    private final Handler h;

    private boolean running = false;
    private boolean scanning = false;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;

    private volatile State state = State.SEARCHING;
    private volatile int payload = 20;                  // bytes per Bluetooth piece
    private volatile int rssi = 0;
    private volatile long lastDataAt = 0;
    private static final int LIVE_ID = -1;      // "#live" messages (not an answer to a request)
    private volatile boolean fast = false;
    private volatile Listener listener;

    private final ArrayDeque<byte[]> writes = new ArrayDeque<>();
    private boolean writing = false;
    private long writeStarted = 0;

    private final Map<Integer, Pending> pending = new ConcurrentHashMap<>();
    private final AtomicInteger nextId = new AtomicInteger(1);
    private final ByteArrayOutputStream inbox = new ByteArrayOutputStream();

    private RodLink(Context context)
    {
        app = context.getApplicationContext();

        BluetoothManager bm = (BluetoothManager) app.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;

        HandlerThread t = new HandlerThread("rodlink");
        t.start();
        h = new Handler(t.getLooper());

        SharedPreferences p = app.getSharedPreferences("rod", Context.MODE_PRIVATE);
        pairedAddress = p.getString("address", "");
        pairedId = p.getString("id", "");
    }

    // ---------- pairing (public) ----------

    boolean paired()
    {
        return !pairedAddress.isEmpty();
    }

    String rodId()
    {
        return pairedId;
    }

    // look for rods open for pairing (the pairing page is on screen)
    void setPairScan(final boolean on)
    {
        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                if(on == pairScan)
                    return;

                pairScan = on;

                if(on)
                    pairSeen.clear();

                if(!paired())
                {
                    stopScan();
                    connectOrScan();
                }
            }
        });
    }

    List<RodSeen> pairList()
    {
        List<RodSeen> l = new ArrayList<>();
        long now = System.currentTimeMillis();

        for(RodSeen r : pairSeen.values())
            if(now - r.at < 15000)
                l.add(r);

        return l;
    }

    PairState pairState()
    {
        return pairState;
    }

    @SuppressLint("MissingPermission")
    void pair(final String address, final String id, final String pin)
    {
        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                pairAddress = address;
                pairIdWanted = id;
                pairPin = pin;
                pairState = PairState.WORKING;
                stopScan();
                listenForBonding();

                try
                {
                    BluetoothDevice d = adapter.getRemoteDevice(address);

                    if(d.getBondState() == BluetoothDevice.BOND_BONDED)
                        removeBond(d);         // an old pairing: start fresh with the PIN

                    if(!d.createBond())
                        pairState = PairState.FAILED;
                }
                catch(Exception e)
                {
                    pairState = PairState.FAILED;
                }
            }
        });
    }

    // "Forget my devices": the rod forgets every phone and watch, and this
    // app forgets the rod (pair again with the PIN)
    void forget()
    {
        if(ready())
            request("/forget", 3000);

        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                try
                {
                    if(!pairedAddress.isEmpty())
                        removeBond(adapter.getRemoteDevice(pairedAddress));
                }
                catch(Exception e)
                {
                }

                savePairing("", "");
                closeGatt();
                stopScan();
                pairState = PairState.IDLE;
                connectOrScan();
            }
        });
    }

    private void savePairing(String address, String id)
    {
        pairedAddress = address;
        pairedId = id;
        app.getSharedPreferences("rod", Context.MODE_PRIVATE).edit().putString("address", address).putString("id", id).apply();
    }

    private static void removeBond(BluetoothDevice d)
    {
        try
        {
            d.getClass().getMethod("removeBond").invoke(d);
        }
        catch(Exception e)
        {
        }
    }

    // Android asks for the PIN: give the one typed in the app; the result of the pairing
    private final BroadcastReceiver bonding = new BroadcastReceiver()
    {
        @SuppressLint("MissingPermission")
        @Override
        public void onReceive(Context c, Intent i)
        {
            BluetoothDevice d = i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);

            if(d == null || !d.getAddress().equalsIgnoreCase(pairAddress))
                return;

            if(BluetoothDevice.ACTION_PAIRING_REQUEST.equals(i.getAction()))
            {
                try
                {
                    d.setPin(pairPin.getBytes(StandardCharsets.UTF_8));
                }
                catch(Exception e)
                {
                    // Android shows its own box: the user types the PIN there
                }
                return;
            }

            int now = i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE);
            int before = i.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, BluetoothDevice.BOND_NONE);

            if(now == BluetoothDevice.BOND_BONDED)
            {
                savePairing(pairAddress, pairIdWanted);
                pairState = PairState.OK;
                pairScan = false;
                kick();
            }
            else if(now == BluetoothDevice.BOND_NONE && before == BluetoothDevice.BOND_BONDING)
            {
                pairState = PairState.FAILED;      // wrong PIN, or the rod's 3 minutes are over
            }
        }
    };

    private void listenForBonding()
    {
        if(receiversOn)
            return;

        receiversOn = true;
        IntentFilter f = new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        f.addAction(BluetoothDevice.ACTION_PAIRING_REQUEST);
        f.setPriority(IntentFilter.SYSTEM_HIGH_PRIORITY - 1);

        if(Build.VERSION.SDK_INT >= 33)
            app.registerReceiver(bonding, f, Context.RECEIVER_EXPORTED);
        else
            app.registerReceiver(bonding, f);
    }

    static synchronized RodLink get(Context context)
    {
        if(instance == null)
            instance = new RodLink(context);

        return instance;
    }

    // ---------- public ----------

    State state()
    {
        return state;
    }

    boolean ready()
    {
        return state == State.CONNECTED;
    }

    int rssi()
    {
        return rssi;
    }

    long lastDataAt()
    {
        return lastDataAt;
    }

    void setListener(Listener l)
    {
        listener = l;
    }

    void start()
    {
        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                if(running)
                    return;

                running = true;
                connectOrScan();
                h.postDelayed(tick, 2000);
            }
        });
    }

    void stop()
    {
        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                running = false;
                stopScan();
                closeGatt();
                h.removeCallbacksAndMessages(null);
            }
        });
    }

    // Look again now (after permissions were given or Bluetooth turned on)
    void kick()
    {
        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                if(running && gatt == null && !scanning)
                    connectOrScan();
            }
        });
    }

    // Quick updates while the dashboard is on screen, slower (saves
    // battery) in the background
    void setFast(final boolean on)
    {
        fast = on;

        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                if(gatt != null && state == State.CONNECTED)
                    gatt.requestConnectionPriority(on ? BluetoothGatt.CONNECTION_PRIORITY_HIGH : BluetoothGatt.CONNECTION_PRIORITY_BALANCED);
            }
        });
    }

    // Ask the rod for a page, e.g. "/data". Blocks; never call it on the main thread.
    Response request(String path, long timeoutMs)
    {
        if(state != State.CONNECTED)
            return null;

        int id = nextId.getAndIncrement() % 500000;      // the watch uses 500000 and up
        Pending p = new Pending(path);
        pending.put(id, p);

        final byte[] line = (id + "|" + path + "\n").getBytes(StandardCharsets.UTF_8);

        h.post(new Runnable()
        {
            @Override
            public void run()
            {
                int n = Math.max(20, Math.min(payload, 512));

                for(int i = 0; i < line.length; i += n)
                {
                    byte[] piece = new byte[Math.min(n, line.length - i)];
                    System.arraycopy(line, i, piece, 0, piece.length);
                    writes.add(piece);
                }

                pumpWrites();
            }
        });

        try
        {
            if(!p.done.await(timeoutMs, TimeUnit.MILLISECONDS))
                return null;
        }
        catch(InterruptedException e)
        {
            return null;
        }
        finally
        {
            pending.remove(id);
        }

        return p.response;
    }

    // ---------- finding and connecting ----------

    private boolean hasPermissions()
    {
        if(Build.VERSION.SDK_INT >= 31)
        {
            return app.checkSelfPermission("android.permission.BLUETOOTH_SCAN") == PackageManager.PERMISSION_GRANTED &&
                   app.checkSelfPermission("android.permission.BLUETOOTH_CONNECT") == PackageManager.PERMISSION_GRANTED;
        }

        return Build.VERSION.SDK_INT < 23 ||
               app.checkSelfPermission("android.permission.ACCESS_FINE_LOCATION") == PackageManager.PERMISSION_GRANTED;
    }

    private final Runnable retry = new Runnable()
    {
        @Override
        public void run()
        {
            connectOrScan();
        }
    };

    private void connectOrScan()
    {
        h.removeCallbacks(retry);

        if(!running || gatt != null)
            return;

        if(!hasPermissions())
        {
            state = State.NO_PERMISSION;
            h.postDelayed(retry, 3000);
            return;
        }

        if(adapter == null || !adapter.isEnabled())
        {
            state = State.BLUETOOTH_OFF;
            h.postDelayed(retry, 3000);
            return;
        }

        if(!paired())
        {
            state = State.NOT_PAIRED;

            if(pairScan)
                startScan();                     // list the rods open for pairing
            else
                h.postDelayed(retry, 3000);

            return;
        }

        startScan();
    }

    private final ScanCallback scanCallback = new ScanCallback()
    {
        @Override
        public void onScanResult(int callbackType, final ScanResult result)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(!scanning || gatt != null)
                        return;

                    // pairing page: list the rods (by their ID), connect to none
                    if(!paired())
                    {
                        String name = result.getScanRecord() != null ? result.getScanRecord().getDeviceName() : null;

                        if(name != null && name.startsWith("ROD-"))
                            pairSeen.put(result.getDevice().getAddress(), new RodSeen(name, result.getDevice().getAddress(), result.getRssi()));

                        return;
                    }

                    // only our own rod (the scan filter already asks for its address)
                    if(!result.getDevice().getAddress().equalsIgnoreCase(pairedAddress))
                        return;

                    stopScan();
                    rssi = result.getRssi();
                    connect(result.getDevice());
                }
            });
        }

        @Override
        public void onScanFailed(int errorCode)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    scanning = false;
                    h.postDelayed(retry, 5000);
                }
            });
        }
    };

    private final Runnable restartScan = new Runnable()
    {
        @Override
        public void run()
        {
            if(scanning)
            {
                stopScan();
                connectOrScan();
            }
        }
    };

    private void startScan()
    {
        scanner = adapter.getBluetoothLeScanner();

        if(scanner == null)
        {
            h.postDelayed(retry, 3000);
            return;
        }

        // paired: our rod's address only (it hides its name after pairing);
        // not paired: rods open for pairing show the StrikeSense service
        ScanFilter filter = paired()
            ? new ScanFilter.Builder().setDeviceAddress(pairedAddress.toUpperCase(java.util.Locale.ROOT)).build()
            : new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SERVICE)).build();
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();

        try
        {
            scanner.startScan(Collections.singletonList(filter), settings, scanCallback);
            scanning = true;
            state = paired() ? State.SEARCHING : State.NOT_PAIRED;
            h.postDelayed(restartScan, SCAN_RESTART_MS);
        }
        catch(Exception e)
        {
            scanning = false;
            h.postDelayed(retry, 5000);
        }
    }

    private void stopScan()
    {
        h.removeCallbacks(restartScan);

        if(scanning && scanner != null)
        {
            try
            {
                scanner.stopScan(scanCallback);
            }
            catch(Exception e)
            {
            }
        }

        scanning = false;
    }

    private final Runnable connectTimeout = new Runnable()
    {
        @Override
        public void run()
        {
            if(state != State.CONNECTED)
                lost();
        }
    };

    private void connect(BluetoothDevice device)
    {
        state = State.CONNECTING;

        if(Build.VERSION.SDK_INT >= 23)
            gatt = device.connectGatt(app, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
        else
            gatt = device.connectGatt(app, false, gattCallback);

        h.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS);
    }

    private void closeGatt()
    {
        h.removeCallbacks(connectTimeout);

        if(gatt != null)
        {
            try
            {
                gatt.disconnect();
                gatt.close();
            }
            catch(Exception e)
            {
            }
        }

        gatt = null;
        rx = null;
        writes.clear();
        writing = false;
        payload = 20;

        synchronized(inbox)
        {
            inbox.reset();
        }

        for(Pending p : pending.values())
            p.done.countDown();

        if(state == State.CONNECTED || state == State.CONNECTING)
            state = State.SEARCHING;
    }

    // Connection gone: clean up and look again
    private void lost()
    {
        closeGatt();

        if(running)
            h.postDelayed(retry, RETRY_MS);
    }

    @SuppressWarnings("deprecation")
    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback()
    {
        @Override
        public void onConnectionStateChange(final BluetoothGatt g, final int status, final int newState)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(g != gatt)
                        return;

                    if(status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED)
                    {
                        // bigger pieces first, then find the rod's service
                        if(!g.requestMtu(517))
                            g.discoverServices();
                    }
                    else
                    {
                        lost();
                    }
                }
            });
        }

        @Override
        public void onMtuChanged(final BluetoothGatt g, final int mtu, final int status)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(g != gatt)
                        return;

                    payload = status == BluetoothGatt.GATT_SUCCESS ? Math.min(512, mtu - 3) : 20;
                    g.discoverServices();
                }
            });
        }

        @Override
        public void onServicesDiscovered(final BluetoothGatt g, int status)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(g != gatt)
                        return;

                    BluetoothGattService s = g.getService(SERVICE);
                    BluetoothGattCharacteristic tx = s != null ? s.getCharacteristic(TX) : null;

                    rx = s != null ? s.getCharacteristic(RX) : null;

                    if(rx == null || tx == null)
                    {
                        lost();
                        return;
                    }

                    g.setCharacteristicNotification(tx, true);

                    BluetoothGattDescriptor d = tx.getDescriptor(CCCD);

                    if(d == null)
                    {
                        lost();
                        return;
                    }

                    if(Build.VERSION.SDK_INT >= 33)
                    {
                        g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    }
                    else
                    {
                        d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                        g.writeDescriptor(d);
                    }
                }
            });
        }

        @Override
        public void onDescriptorWrite(final BluetoothGatt g, BluetoothGattDescriptor d, final int status)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(g != gatt)
                        return;

                    if(status != BluetoothGatt.GATT_SUCCESS)
                    {
                        lost();
                        return;
                    }

                    h.removeCallbacks(connectTimeout);
                    state = State.CONNECTED;

                    g.requestConnectionPriority(fast ? BluetoothGatt.CONNECTION_PRIORITY_HIGH : BluetoothGatt.CONNECTION_PRIORITY_BALANCED);

                    // Tell the rod how big a piece may be
                    final int n = payload;

                    new Thread(new Runnable()
                    {
                        @Override
                        public void run()
                        {
                            request("/mtu?v=" + n, 3000);

                            // the time and date (with summer / winter time):
                            // the rod passes them on to the watch
                            long now = System.currentTimeMillis();
                            int tz = java.util.TimeZone.getDefault().getOffset(now) / 60000;
                            request("/settime?t=" + (now / 1000) + "&tz=" + tz, 3000);
                        }
                    }).start();
                }
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status)
        {
            h.post(new Runnable()
            {
                @Override
                public void run()
                {
                    writing = false;
                    pumpWrites();
                }
            });
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value)
        {
            received(value);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c)
        {
            received(c.getValue());
        }

        @Override
        public void onReadRemoteRssi(BluetoothGatt g, int value, int status)
        {
            if(status == BluetoothGatt.GATT_SUCCESS)
                rssi = value;
        }
    };

    // ---------- sending ----------

    @SuppressWarnings("deprecation")
    private void pumpWrites()
    {
        // a write that never finished must not block everything
        if(writing && System.currentTimeMillis() - writeStarted > 1000)
            writing = false;

        if(writing || writes.isEmpty() || gatt == null || rx == null)
            return;

        byte[] piece = writes.peek();
        boolean ok;

        if(Build.VERSION.SDK_INT >= 33)
        {
            ok = gatt.writeCharacteristic(rx, piece, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == 0;
        }
        else
        {
            rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            rx.setValue(piece);
            ok = gatt.writeCharacteristic(rx);
        }

        if(ok)
        {
            writes.poll();
            writing = true;
            writeStarted = System.currentTimeMillis();
        }
        else
        {
            h.postDelayed(new Runnable()
            {
                @Override
                public void run()
                {
                    pumpWrites();
                }
            }, 15);
        }
    }

    // ---------- receiving ----------

    private void received(byte[] value)
    {
        if(value == null)
            return;

        synchronized(inbox)
        {
            inbox.write(value, 0, value.length);

            while(true)
            {
                byte[] buf = inbox.toByteArray();

                // Each answer starts with '#'; skip anything before it
                int start = 0;

                while(start < buf.length && buf[start] != '#')
                    start++;

                int nl = -1;

                for(int i = start; i < buf.length; i++)
                {
                    if(buf[i] == '\n')
                    {
                        nl = i;
                        break;
                    }
                }

                if(nl < 0)
                {
                    keep(buf, start);
                    return;
                }

                String[] head = new String(buf, start + 1, nl - start - 1, StandardCharsets.UTF_8).trim().split(" ");
                int id, code, len;

                try
                {
                    // "#live 200 <len>": the rod's live message, 10 a second, for everyone
                    id = head[0].equals("live") ? LIVE_ID : Integer.parseInt(head[0]);
                    code = Integer.parseInt(head[1]);
                    len = Integer.parseInt(head[2]);
                }
                catch(Exception e)
                {
                    keep(buf, nl + 1);        // broken header: drop it
                    continue;
                }

                if(buf.length - (nl + 1) < len)
                {
                    keep(buf, start);         // body not complete yet
                    return;
                }

                byte[] body = new byte[len];
                System.arraycopy(buf, nl + 1, body, 0, len);
                keep(buf, nl + 1 + len);

                answer(id, code, body);
            }
        }
    }

    private void keep(byte[] buf, int from)
    {
        inbox.reset();

        if(from < buf.length)
            inbox.write(buf, from, buf.length - from);
    }

    private void answer(int id, int code, byte[] body)
    {
        // the live message drives the background alarm: no need to ask for /data
        if(id == LIVE_ID)
        {
            if(code == 200)
            {
                lastDataAt = System.currentTimeMillis();

                Listener l = listener;

                if(l != null)
                    l.onData(new String(body, StandardCharsets.UTF_8));
            }

            return;
        }

        Pending p = pending.get(id);

        if(p == null)
            return;

        p.response = new Response(code, body);
        p.done.countDown();

        if(code == 200 && p.path.startsWith("/data"))
        {
            lastDataAt = System.currentTimeMillis();

            Listener l = listener;

            if(l != null)
                l.onData(new String(body, StandardCharsets.UTF_8));
        }
    }

    // ---------- every 2 s: signal strength ----------

    private final Runnable tick = new Runnable()
    {
        @Override
        public void run()
        {
            if(!running)
                return;

            if(gatt != null && state == State.CONNECTED)
            {
                try
                {
                    gatt.readRemoteRssi();
                }
                catch(Exception e)
                {
                }
            }

            h.postDelayed(this, 2000);
        }
    };
}
