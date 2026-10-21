package com.laarif.strikesense;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.VibrationEffect;
import android.os.Vibrator;

import org.json.JSONObject;

// Watches the rod over Bluetooth in the background (also with the screen
// off) and sounds an alarm with vibration when a strike comes in.
public class RodService extends Service
{
    static final String ACTION_STOP = "com.laarif.strikesense.STOP";
    static final String ACTION_WATCHING = "com.laarif.strikesense.WATCHING";
    static final String ACTION_SILENCE = "com.laarif.strikesense.SILENCE";
    static final String ACTION_APP_VISIBLE = "com.laarif.strikesense.APP_VISIBLE";

    private static final String CH_WATCH = "watch";
    private static final String CH_STRIKE = "strike";
    private static final String CH_INFO = "info";

    private static final int ID_WATCH = 1;
    private static final int ID_STRIKE = 2;
    private static final int ID_INFO = 3;

    private static final int POLL_MS = 1000;
    private static final long LOST_MS = 30000;            // no answer this long = connection lost

    private static final long[] STRIKE_BUZZ = { 0, 600, 300, 600, 300, 1200, 700 };

    private HandlerThread thread;
    private Handler worker;
    private NotificationManager nm;
    private PowerManager.WakeLock wakeLock;
    private MediaPlayer player;
    private Vibrator vibrator;

    private boolean running = false;
    private boolean everConnected = false;
    private boolean lostNotified = false;
    private boolean alarming = false;
    private boolean batteryLowNotified = false;
    private long lostSince = 0;
    private long lastStrikeId = Long.MIN_VALUE;
    private String lastStatus = "";

    @Override
    public void onCreate()
    {
        super.onCreate();

        nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);

        makeChannels();
        goForeground("Looking for the rod…");

        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "StrikeSense:watch");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();

        thread = new HandlerThread("rod");
        thread.start();
        worker = new Handler(thread.getLooper());

        // Every answer from the rod (also the dashboard's) is checked here
        RodLink link = RodLink.get(this);

        link.setListener(new RodLink.Listener()
        {
            @Override
            public void onData(final String json)
            {
                worker.post(new Runnable()
                {
                    @Override
                    public void run()
                    {
                        try
                        {
                            handle(new JSONObject(json));
                        }
                        catch(Exception e)
                        {
                        }
                    }
                });
            }
        });

        link.start();

        running = true;
        worker.post(poll);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        String action = intent != null ? intent.getAction() : null;

        if(ACTION_STOP.equals(action))
        {
            stopAlarm(true);
            stopSelf();
        }
        else if(ACTION_WATCHING.equals(action))
        {
            // Same as "I'M WATCHING" on the dashboard
            worker.post(new Runnable()
            {
                @Override
                public void run()
                {
                    RodLink.get(RodService.this).request("/watching", 3000);
                    stopAlarm(true);
                }
            });
        }
        else if(ACTION_SILENCE.equals(action) || ACTION_APP_VISIBLE.equals(action))
        {
            silence();
        }

        return START_STICKY;
    }

    // ---------- polling ----------

    private final Runnable poll = new Runnable()
    {
        @Override
        public void run()
        {
            if(!running)
                return;

            try
            {
                check();
            }
            catch(Exception e)
            {
            }

            worker.postDelayed(this, POLL_MS);
        }
    };

    // Ask for /data only when the dashboard isn't already doing it
    private void check()
    {
        RodLink link = RodLink.get(this);
        long now = System.currentTimeMillis();

        if(link.ready())
        {
            lostSince = 0;

            if(now - link.lastDataAt() > 1500)
                link.request("/data", 3000);       // the answer arrives in handle()

            return;
        }

        if(lostSince == 0)
            lostSince = now;

        if(everConnected && now - lostSince > LOST_MS && !lostNotified)
        {
            lostNotified = true;
            info("Lost connection to the rod",
                 "No Bluetooth contact for 30 s. Check the rod's battery and come closer to the rod (about 30 m).", true);
        }

        if(!everConnected || now - lostSince > 5000)
            status(link.state() == RodLink.State.BLUETOOTH_OFF ? "Bluetooth is off – turn it on"
                 : link.state() == RodLink.State.NO_PERMISSION ? "Open the app to allow Bluetooth"
                 : "Looking for the rod (Bluetooth)…");
    }

    private void handle(JSONObject d)
    {
        long now = System.currentTimeMillis();

        if(lostNotified)
            nm.cancel(ID_INFO);

        everConnected = true;
        lostNotified = false;

        boolean strike = d.optBoolean("strike", false);
        long strikeId = d.optLong("strikeid", -1);

        if(strike && strikeId != lastStrikeId)
        {
            lastStrikeId = strikeId;
            startAlarm(d.optString("type", "STRIKE"));
        }
        else if(!strike && alarming)
        {
            stopAlarm(true);         // answered on the dashboard or the watch
        }

        int bat = d.optInt("bat", -1);

        if(bat >= 0 && bat < 15 && !batteryLowNotified)
        {
            batteryLowNotified = true;
            info("Rod battery low", "Battery at " + bat + " %. Charge it soon.", false);
        }
        else if(bat > 25)
        {
            batteryLowNotified = false;
        }

        status(stateText(d, strike) + (bat >= 0 ? " · battery " + bat + "%" : ""));
    }

    private static String stateText(JSONObject d, boolean strike)
    {
        if(strike)
            return "STRIKE! " + d.optString("type", "");

        String s = d.optString("state", "");

        if(s.equals("monitoring")) return "Rod armed – watching";
        if(s.equals("learning")) return "Learning the sea – keep the rod still";
        if(s.equals("pulltest")) return "Pull test – open the app";
        if(s.equals("settling")) return "Settling";
        if(s.equals("paused")) return "Reeled in – press READY after casting";

        return "Connected";
    }

    // ---------- alarm ----------

    // The four alarm sounds (assets/sounds/*.ogg) and the default per alarm type
    static final java.util.List<String> SOUNDS =
        java.util.Arrays.asList("reel_scream", "reel_clicks", "rod_bell", "falling_tone");

    private String soundFile(String type)
    {
        String cat = type.equals("SLOW PULL") ? "slow"
                   : type.equals("REPEATED BITE") ? "knock"
                   : type.startsWith("DROP") ? "drop"
                   : "strike";

        String def = cat.equals("slow") ? "reel_clicks"
                   : cat.equals("knock") ? "rod_bell"
                   : cat.equals("drop") ? "falling_tone"
                   : "reel_scream";

        String name = getSharedPreferences("sounds", MODE_PRIVATE).getString(cat, def);

        return SOUNDS.contains(name) ? name : def;
    }

    private synchronized void startAlarm(String type)
    {
        alarming = true;

        Notification.Builder b = builder(CH_STRIKE)
            .setSmallIcon(R.drawable.ic_notify)
            .setContentTitle("STRIKE! " + type)
            .setContentText("Fish on – check your rod")
            .setCategory(Notification.CATEGORY_ALARM)
            .setOngoing(true)
            .setAutoCancel(false)
            .setColor(0xFFFF4D5E)
            .setContentIntent(openApp(0))
            .setFullScreenIntent(openApp(1), true)
            .addAction(0, "I'M WATCHING", action(ACTION_WATCHING, 2))
            .addAction(0, "SILENCE", action(ACTION_SILENCE, 3));

        if(Build.VERSION.SDK_INT < 26)
            b.setPriority(Notification.PRIORITY_MAX);

        nm.notify(ID_STRIKE, b.build());

        if(MainActivity.visible)
        {
            buzz(new long[] { 0, 400 }, -1);     // the dashboard plays its own sound
            return;
        }

        buzz(STRIKE_BUZZ, 0);

        try
        {
            stopSound();
            player = new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ALARM)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build());

            try
            {
                // the chosen fishing sound
                android.content.res.AssetFileDescriptor fd = getAssets().openFd("sounds/" + soundFile(type) + ".ogg");
                player.setDataSource(fd.getFileDescriptor(), fd.getStartOffset(), fd.getLength());
                fd.close();
            }
            catch(Exception e)
            {
                // not there: the phone's alarm sound, as before
                Uri uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM);

                if(uri == null)
                    uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);

                player.reset();
                player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ALARM)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build());
                player.setDataSource(this, uri);
            }

            player.setLooping(true);
            player.prepare();
            player.start();
        }
        catch(Exception e)
        {
            stopSound();
        }
    }

    private synchronized void silence()
    {
        stopSound();

        if(vibrator != null)
            vibrator.cancel();
    }

    private synchronized void stopAlarm(boolean clear)
    {
        alarming = false;
        silence();

        if(clear)
            nm.cancel(ID_STRIKE);
    }

    private synchronized void stopSound()
    {
        if(player != null)
        {
            try
            {
                player.stop();
            }
            catch(Exception e)
            {
            }

            player.release();
            player = null;
        }
    }

    @SuppressWarnings("deprecation")
    private void buzz(long[] pattern, int repeat)
    {
        if(vibrator == null)
            return;

        if(Build.VERSION.SDK_INT >= 26)
            vibrator.vibrate(VibrationEffect.createWaveform(pattern, repeat));
        else
            vibrator.vibrate(pattern, repeat);
    }

    // ---------- notifications ----------

    private void makeChannels()
    {
        if(Build.VERSION.SDK_INT < 26)
            return;

        NotificationChannel watch = new NotificationChannel(CH_WATCH, "Watching the rod", NotificationManager.IMPORTANCE_LOW);
        watch.setDescription("Shows that StrikeSense is watching your rod");
        watch.setShowBadge(false);

        NotificationChannel strike = new NotificationChannel(CH_STRIKE, "Strike alarm", NotificationManager.IMPORTANCE_HIGH);
        strike.setDescription("Fish on! Rings and vibrates");
        strike.setSound(null, null);               // the service plays the alarm itself (loops until answered)
        strike.enableVibration(false);
        strike.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        strike.setBypassDnd(true);

        NotificationChannel info = new NotificationChannel(CH_INFO, "Rod warnings", NotificationManager.IMPORTANCE_HIGH);
        info.setDescription("Lost connection, low battery");
        info.enableVibration(true);

        nm.createNotificationChannel(watch);
        nm.createNotificationChannel(strike);
        nm.createNotificationChannel(info);
    }

    @SuppressWarnings("deprecation")
    private Notification.Builder builder(String channel)
    {
        if(Build.VERSION.SDK_INT >= 26)
            return new Notification.Builder(this, channel);

        return new Notification.Builder(this);
    }

    private Notification watchNotification(String text)
    {
        Notification.Builder b = builder(CH_WATCH)
            .setSmallIcon(R.drawable.ic_notify)
            .setContentTitle("StrikeSense")
            .setContentText(text)
            .setOngoing(true)
            .setShowWhen(false)
            .setColor(0xFF22D3FF)
            .setContentIntent(openApp(4))
            .addAction(0, "STOP WATCHING", action(ACTION_STOP, 5));

        return b.build();
    }

    private void goForeground(String text)
    {
        try
        {
            if(Build.VERSION.SDK_INT >= 29)
                startForeground(ID_WATCH, watchNotification(text), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else
                startForeground(ID_WATCH, watchNotification(text));
        }
        catch(Exception e)
        {
        }
    }

    private void status(String text)
    {
        if(text.equals(lastStatus))
            return;

        lastStatus = text;
        nm.notify(ID_WATCH, watchNotification(text));
    }

    @SuppressWarnings("deprecation")
    private void info(String title, String text, boolean vibrate)
    {
        Notification.Builder b = builder(CH_INFO)
            .setSmallIcon(R.drawable.ic_notify)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(new Notification.BigTextStyle().bigText(text))
            .setColor(0xFFFFBA52)
            .setAutoCancel(true)
            .setContentIntent(openApp(6));

        if(Build.VERSION.SDK_INT < 26)
            b.setPriority(Notification.PRIORITY_HIGH);

        nm.notify(ID_INFO, b.build());

        if(vibrate && !MainActivity.visible)
            buzz(new long[] { 0, 300, 200, 300 }, -1);
    }

    private int piFlags()
    {
        return Build.VERSION.SDK_INT >= 23
            ? PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
            : PendingIntent.FLAG_UPDATE_CURRENT;
    }

    private PendingIntent openApp(int code)
    {
        Intent i = new Intent(this, MainActivity.class)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);

        return PendingIntent.getActivity(this, code, i, piFlags());
    }

    private PendingIntent action(String name, int code)
    {
        return PendingIntent.getService(this, code, new Intent(this, RodService.class).setAction(name), piFlags());
    }

    // ---------- end ----------

    @Override
    public void onDestroy()
    {
        running = false;
        stopAlarm(true);

        if(worker != null)
            worker.removeCallbacksAndMessages(null);

        if(thread != null)
            thread.quitSafely();

        if(wakeLock != null && wakeLock.isHeld())
            wakeLock.release();

        RodLink.get(this).setListener(null);
        RodLink.get(this).stop();

        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }
}
