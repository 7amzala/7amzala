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
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelUuid;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Collections;
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

    enum State { NO_PERMISSION, BLUETOOTH_OFF, SEARCHING, CONNECTING, CONNECTED }

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

        int id = nextId.getAndIncrement() % 1000000;
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

        ScanFilter filter = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SERVICE)).build();
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();

        try
        {
            scanner.startScan(Collections.singletonList(filter), settings, scanCallback);
            scanning = true;
            state = State.SEARCHING;
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
                    id = Integer.parseInt(head[0]);
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
