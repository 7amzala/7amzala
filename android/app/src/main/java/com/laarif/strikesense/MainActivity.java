package com.laarif.strikesense;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.provider.Settings;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

// The dashboard (built into the app, the same page the rod used to serve)
// in full screen. Its requests (/data, /set ...) go to the rod over
// Bluetooth. A help page shows until the rod is found.
public class MainActivity extends Activity
{
    // A made-up address: every request to it is answered by the app
    static final String HOST = "rod.strikesense";
    static final String DASHBOARD_URL = "http://" + HOST + "/";
    private static final String HELP_URL = "file:///android_asset/offline.html";
    private static final String PAIR_URL = DASHBOARD_URL + "app/pair.html";
    private static final String ACCOUNT_URL = DASHBOARD_URL + "app/account.html";
    private boolean onPairPage = false;
    private boolean onAccountPage = false;
    private volatile String accountResult = "";

    private static final int REQ_PERMISSIONS = 1;
    private static final int REQ_BLUETOOTH_ON = 2;
    private static final int REQ_PHOTO = 3;
    private static final int REQ_PROFILE = 4;

    // the catch photo being taken (camera only: a real photo, taken now)
    private String photoUid = "", photoSlot = "";
    private long photoAskedAt = 0;
    private static final long PHOTO_MAX_AGE_MS = 10 * 60000L;   // the camera's own time stamp: at most 10 min off

    static volatile boolean visible = false;

    private WebView web;
    private WebView strikes;                 // MY STRIKES and the map, on top of the dashboard (which keeps running)

    // bottom tabs
    private static final int TAB_ROD = 0, TAB_STRIKES = 1, TAB_MAP = 2, TAB_SETTINGS = 3;
    private final TextView[] tabs = new TextView[4];
    private int tab = TAB_ROD;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean onDashboard = false;
    private boolean askedBluetoothOn = false;

    @SuppressLint({ "SetJavaScriptEnabled", "AddJavascriptInterface" })
    @Override
    protected void onCreate(Bundle state)
    {
        super.onCreate(state);

        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        window.setStatusBarColor(Color.rgb(3, 8, 20));
        window.setNavigationBarColor(Color.rgb(3, 8, 20));

        web = new WebView(this);
        web.setBackgroundColor(Color.rgb(3, 8, 20));

        FrameLayout pages = new FrameLayout(this);
        pages.addView(web);
        makeStrikesView(pages);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(3, 8, 20));
        root.addView(pages, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        root.addView(makeTabs(), new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(58)));
        setContentView(root);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);    // alarm sounds work without tapping first
        s.setTextZoom(100);                              // big system font must not break the layout

        web.addJavascriptInterface(new Bridge(), "StrikeApp");
        web.setWebChromeClient(new WebChromeClient());
        web.setWebViewClient(new Client());

        web.loadUrl(HELP_URL);

        if(askPermissions())
            permissionsReady();

        handler.post(watch);
    }

    // ---------- permissions ----------

    // true = all there already
    private boolean askPermissions()
    {
        List<String> need = new ArrayList<>();

        if(Build.VERSION.SDK_INT >= 31)
        {
            need.add("android.permission.BLUETOOTH_SCAN");
            need.add("android.permission.BLUETOOTH_CONNECT");
        }
        else
        {
            need.add("android.permission.ACCESS_FINE_LOCATION");     // older Android: needed to find Bluetooth devices
        }

        if(Build.VERSION.SDK_INT >= 33)
            need.add("android.permission.POST_NOTIFICATIONS");

        // where each strike was (MY STRIKES): asked once, works without it too
        SharedPreferences prefs = getSharedPreferences("app", MODE_PRIVATE);

        if(Build.VERSION.SDK_INT >= 31 && !prefs.getBoolean("askedGps", false))
        {
            prefs.edit().putBoolean("askedGps", true).apply();
            need.add("android.permission.ACCESS_FINE_LOCATION");
            need.add("android.permission.ACCESS_COARSE_LOCATION");
        }

        List<String> missing = new ArrayList<>();

        for(String p : need)
        {
            if(checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED && !missing.contains(p))
                missing.add(p);
        }

        if(missing.isEmpty())
            return true;

        requestPermissions(missing.toArray(new String[0]), REQ_PERMISSIONS);
        return false;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results)
    {
        super.onRequestPermissionsResult(code, permissions, results);

        if(code == REQ_PERMISSIONS)
            permissionsReady();
    }

    private void permissionsReady()
    {
        startRodService();
        askBluetoothOn();
        askBatteryOnce();
        RodLink.get(this).kick();
    }

    @SuppressLint("MissingPermission")
    private void askBluetoothOn()
    {
        BluetoothManager bm = (BluetoothManager) getSystemService(BLUETOOTH_SERVICE);
        BluetoothAdapter a = bm != null ? bm.getAdapter() : null;

        if(a == null || a.isEnabled() || askedBluetoothOn)
            return;

        askedBluetoothOn = true;

        try
        {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_BLUETOOTH_ON);
        }
        catch(Exception e)
        {
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data)
    {
        super.onActivityResult(code, result, data);

        if(code == REQ_PHOTO)
        {
            gotPhoto(result == RESULT_OK, data);
            return;
        }

        if(code == REQ_PROFILE)
        {
            gotProfile(result == RESULT_OK, data);
            return;
        }

        RodLink.get(this).kick();
    }

    // ---------- catch photos (hook in the mouth, weight, length) ----------

    // Camera only (no gallery): the photo is taken now, so it is real
    private void takePhoto(String uid, String slot)
    {
        photoUid = uid;
        photoSlot = slot;
        photoAskedAt = System.currentTimeMillis();

        // the phone may close the app while the camera is open: remember which photo it was
        getSharedPreferences("app", MODE_PRIVATE).edit().putString("photoUid", uid).putString("photoSlot", slot)
            .putLong("photoAskedAt", photoAskedAt).apply();

        try
        {
            PhotoProvider.cameraFile(this).delete();
            Uri out = PhotoProvider.cameraUri();
            Intent i = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
            i.putExtra(MediaStore.EXTRA_OUTPUT, out);
            i.setClipData(ClipData.newRawUri("", out));
            i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(i, REQ_PHOTO);
        }
        catch(Exception e)
        {
            photoDone(false, "No camera app on this phone");
        }
    }

    // A real photo, taken now: the camera wrote our file after we opened it,
    // and the camera's own time stamp (when it has one) is now
    private String notNew(File f, long askedAt)
    {
        if(f.lastModified() < askedAt - 5000)
            return "This photo is not new. Take the photo now.";

        try
        {
            FileInputStream in = new FileInputStream(f);
            ExifInterface ex = new ExifInterface(in);
            in.close();

            String t = ex.getAttribute(ExifInterface.TAG_DATETIME_ORIGINAL);

            if(t == null)
                t = ex.getAttribute(ExifInterface.TAG_DATETIME);

            if(t != null && t.length() >= 19)
            {
                java.text.SimpleDateFormat fmt = new java.text.SimpleDateFormat("yyyy:MM:dd HH:mm:ss", java.util.Locale.US);
                long at = fmt.parse(t.substring(0, 19)).getTime();      // the phone's local time

                if(Math.abs(at - System.currentTimeMillis()) > PHOTO_MAX_AGE_MS)
                    return "This photo is not new (taken " + t.substring(0, 16).replace(':', '-').replaceFirst("-(\\d\\d)$", ":$1") + "). Take the photo now.";
            }
        }
        catch(Exception e)
        {
        }

        return "";
    }

    private void gotPhoto(final boolean ok, final Intent data)
    {
        if(photoUid.isEmpty())
        {
            SharedPreferences p = getSharedPreferences("app", MODE_PRIVATE);
            photoUid = p.getString("photoUid", "");
            photoSlot = p.getString("photoSlot", "");
            photoAskedAt = p.getLong("photoAskedAt", 0);
        }

        final String uid = photoUid, slot = photoSlot;
        final long askedAt = photoAskedAt;

        if(!ok || uid.isEmpty())
        {
            photoDone(false, "");
            return;
        }

        new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                StrikeStore st = StrikeStore.get(MainActivity.this);
                boolean saved = false;
                String why = "Could not read that photo";

                try
                {
                    File camFile = PhotoProvider.cameraFile(MainActivity.this);

                    if(!camFile.exists() || camFile.length() == 0)
                        throw new Exception("no photo");

                    why = notNew(camFile, askedAt);

                    if(why.isEmpty())
                        saved = smallJpeg(null, camFile, st.photoFile(uid, slot));

                    camFile.delete();
                }
                catch(Exception e)
                {
                    saved = false;
                }

                if(saved)
                    st.photoSaved(uid, slot, System.currentTimeMillis());

                final boolean done = saved;
                final String note = why.isEmpty() ? "Could not read that photo" : why;

                handler.post(new Runnable()
                {
                    @Override
                    public void run()
                    {
                        photoDone(done, done ? "" : note);
                    }
                });

                if(saved)
                    st.syncNow(true);                // online now if there is internet
            }
        }).start();
    }

    private void photoDone(boolean ok, String note)
    {
        strikes.evaluateJavascript("window.photoDone && photoDone(" + quote(photoSlot) + "," + ok + "," + quote(note) + ")", null);
    }

    // The photo made small: longest side 1600 px, JPEG ~200-400 KB, turned the right way up
    private boolean smallJpeg(Uri src, File camFile, File out) throws Exception
    {
        return smallJpeg(src, camFile, out, 1600);
    }

    private boolean smallJpeg(Uri src, File camFile, File out, int maxSide) throws Exception
    {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        InputStream in = open(src, camFile);
        BitmapFactory.decodeStream(in, null, o);
        in.close();

        if(o.outWidth <= 0 || o.outHeight <= 0)
            return false;

        int side = Math.max(o.outWidth, o.outHeight);
        int sample = 1;

        while(side / (sample * 2) >= maxSide)
            sample *= 2;

        BitmapFactory.Options d = new BitmapFactory.Options();
        d.inSampleSize = sample;
        in = open(src, camFile);
        Bitmap b = BitmapFactory.decodeStream(in, null, d);
        in.close();

        if(b == null)
            return false;

        // which way up the camera held it
        int turn = 0;

        try
        {
            in = open(src, camFile);
            int or = new ExifInterface(in).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
            in.close();
            turn = or == ExifInterface.ORIENTATION_ROTATE_90 ? 90 : or == ExifInterface.ORIENTATION_ROTATE_180 ? 180
                 : or == ExifInterface.ORIENTATION_ROTATE_270 ? 270 : 0;
        }
        catch(Exception e)
        {
        }

        float scale = Math.min(1f, (float) maxSide / Math.max(b.getWidth(), b.getHeight()));
        Matrix m = new Matrix();
        m.postScale(scale, scale);
        m.postRotate(turn);
        Bitmap small = Bitmap.createBitmap(b, 0, 0, b.getWidth(), b.getHeight(), m, true);

        FileOutputStream f = new FileOutputStream(out);
        boolean ok = small.compress(Bitmap.CompressFormat.JPEG, 82, f);
        f.close();

        if(small != b)
            small.recycle();
        b.recycle();
        return ok;
    }

    private InputStream open(Uri src, File camFile) throws Exception
    {
        return src != null ? getContentResolver().openInputStream(src) : new FileInputStream(camFile);
    }

    // ---------- profile picture (a selfie, or one from the gallery) ----------

    static File profileFile(android.content.Context c)
    {
        return new File(c.getFilesDir(), "profile.jpg");
    }

    private void pickProfile(boolean camera)
    {
        try
        {
            if(camera)
            {
                PhotoProvider.cameraFile(this).delete();
                Uri out = PhotoProvider.cameraUri();
                Intent i = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
                i.putExtra(MediaStore.EXTRA_OUTPUT, out);
                i.setClipData(ClipData.newRawUri("", out));
                i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startActivityForResult(i, REQ_PROFILE);
            }
            else
            {
                Intent i = new Intent(Intent.ACTION_GET_CONTENT);
                i.setType("image/*");
                i.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(i, REQ_PROFILE);
            }
        }
        catch(Exception e)
        {
        }
    }

    private void gotProfile(final boolean ok, final Intent data)
    {
        if(!ok)
            return;

        new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                boolean saved = false;

                try
                {
                    Uri src = data != null ? data.getData() : null;
                    File camFile = PhotoProvider.cameraFile(MainActivity.this);

                    if(src != null || camFile.exists())
                        saved = smallJpeg(src, camFile, profileFile(MainActivity.this), 512);

                    camFile.delete();
                }
                catch(Exception e)
                {
                    saved = false;
                }

                if(saved)
                {
                    handler.post(new Runnable()
                    {
                        @Override
                        public void run()
                        {
                            strikes.evaluateJavascript("window.profileDone && profileDone()", null);
                            web.evaluateJavascript("window.profileDone && profileDone()", null);
                        }
                    });
                }
            }
        }).start();
    }

    // FISH OUT in the app (or ADD PHOTOS): the catch page of the newest strike
    private void openCatch()
    {
        strikes.setVisibility(View.VISIBLE);
        strikes.loadUrl(DASHBOARD_URL + "app/strikes.html#catch");
        lightTab(TAB_STRIKES);
    }

    // Once: let the app keep watching the rod with the screen off
    private void askBatteryOnce()
    {
        SharedPreferences prefs = getSharedPreferences("app", MODE_PRIVATE);
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);

        if(prefs.getBoolean("askedBattery", false) || pm == null || pm.isIgnoringBatteryOptimizations(getPackageName()))
            return;

        prefs.edit().putBoolean("askedBattery", true).apply();

        try
        {
            @SuppressLint("BatteryLife")
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                  Uri.parse("package:" + getPackageName()));
            startActivity(i);
        }
        catch(Exception e)
        {
        }
    }

    private void startRodService()
    {
        Intent i = new Intent(this, RodService.class);

        try
        {
            if(Build.VERSION.SDK_INT >= 26)
                startForegroundService(i);
            else
                startService(i);
        }
        catch(Exception e)
        {
        }
    }

    // ---------- every second: help page or dashboard, signal strength ----------

    private final Runnable watch = new Runnable()
    {
        @Override
        public void run()
        {
            RodLink link = RodLink.get(MainActivity.this);
            boolean watchOnly = RodService.isWatchOnly(MainActivity.this);

            // first start: register / log in (the strikes go online with the username)
            if(needsAccount())
            {
                if(!onAccountPage)
                {
                    onAccountPage = true;
                    onDashboard = false;
                    onPairPage = false;
                    web.loadUrl(ACCOUNT_URL);
                }

                handler.postDelayed(this, 1000);
                return;
            }

            if(onAccountPage)
            {
                onAccountPage = false;
                onDashboard = false;
                web.loadUrl(HELP_URL);
            }

            if(!onDashboard)
            {
                if(link.ready() || (watchOnly && link.paired()))     // Watch only: the dashboard, so the phone can come back
                {
                    onDashboard = true;
                    onPairPage = false;
                    web.loadUrl(DASHBOARD_URL);
                }
                else if(!link.paired() && link.state() == RodLink.State.NOT_PAIRED)
                {
                    // no rod yet: PAIR YOUR ROD
                    if(!onPairPage)
                    {
                        onPairPage = true;
                        web.loadUrl(PAIR_URL);
                    }
                }
                else if(onPairPage && link.paired())
                {
                    // just paired: the page says so until the rod connects
                }
                else
                {
                    js("if(window.setStatus) setStatus(" + quote(helpStatus(link.state())) + ");"
                       + " if(window.showRepair) showRepair(" + link.pairingLost() + ")");
                }
            }
            else
            {
                // coloured signal bars (dashboard 2.3+); older page: the text
                js("if(window.setLink) setLink(" + link.ready() + "," + link.rssi() + ");"
                   + " else { var e=document.getElementById('linkInfo'); if(e) e.textContent=" + quote(linkText(link)) + "; }"
                   + " if(window.setWatchOnlyView) setWatchOnlyView(" + watchOnly + "," + quote(RodService.watchOnlyInfo) + ");");
            }

            handler.postDelayed(this, 1000);
        }
    };

    private boolean accountAsked = false;          // Settings > SIGN IN: show the page again

    private boolean needsAccount()
    {
        if(accountAsked)
            return true;

        return !StrikeStore.get(this).signedIn() && !getSharedPreferences("app", MODE_PRIVATE).getBoolean("accountLater", false);
    }

    private static String helpStatus(RodLink.State s)
    {
        switch(s)
        {
            case NO_PERMISSION: return "Allow Bluetooth for StrikeSense (tap the button below)";
            case BLUETOOTH_OFF: return "Bluetooth is off – turn it on";
            case CONNECTING:    return "Rod found – connecting…";
            case CONNECTED:     return "Connected";
            default:            return "Looking for the rod…";
        }
    }

    private static String linkText(RodLink link)
    {
        if(!link.ready())
            return "Bluetooth · reconnecting…";

        int r = link.rssi();
        String bars = r == 0 ? "" : r >= -60 ? " ▂▄▆█" : r >= -70 ? " ▂▄▆" : r >= -80 ? " ▂▄" : " ▂";

        return "Bluetooth" + bars;
    }

    private static String quote(String t)
    {
        return "'" + t.replace("\\", "\\\\").replace("'", "\\'") + "'";
    }

    private void js(String code)
    {
        web.evaluateJavascript(code, null);
    }

    // ---------- page requests -> Bluetooth ----------

    private class Client extends WebViewClient
    {
        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request)
        {
            Uri u = request.getUrl();

            if(!HOST.equals(u.getHost()))
                return null;

            String path = u.getEncodedPath() == null ? "/" : u.getEncodedPath();

            try
            {
                if(path.equals("/") || path.equals("/index.html"))
                    return asset("dashboard.html", "text/html");

                if(path.equals("/logo.webp"))
                    return asset("logo.webp", "image/webp");

                // the alarm sounds (screaming reel, reel clicks, rod bell, falling tone)
                if(path.matches("/sounds/[a-z_]+\\.ogg"))
                    return asset(path.substring(1), "audio/ogg");

                // MY STRIKES pages (and the map library), in the app
                if(path.matches("/app/[a-z_]+\\.(html|js|css|png)"))
                    return asset(path.substring(1), path.endsWith(".html") ? "text/html"
                                 : path.endsWith(".js") ? "application/javascript"
                                 : path.endsWith(".css") ? "text/css" : "image/png");

                // the user's profile picture
                if(path.equals("/photo/profile.jpg"))
                {
                    File f = profileFile(MainActivity.this);

                    if(!f.exists())
                        return answer(404, "text/plain", new byte[0]);

                    Map<String, String> headers = new HashMap<>();
                    headers.put("Cache-Control", "no-store");
                    return new WebResourceResponse("image/jpeg", null, 200, "OK", headers, new FileInputStream(f));
                }

                // a catch photo kept on the phone
                if(path.matches("/photo/[A-Za-z0-9_-]+_(hook|weight|length)\\.jpg"))
                {
                    String name = path.substring(7, path.length() - 4);
                    int cut = name.lastIndexOf('_');
                    File f = StrikeStore.get(MainActivity.this).photoFile(name.substring(0, cut), name.substring(cut + 1));

                    if(!f.exists())
                        return answer(404, "text/plain", new byte[0]);

                    Map<String, String> headers = new HashMap<>();
                    headers.put("Cache-Control", "no-store");
                    return new WebResourceResponse("image/jpeg", null, 200, "OK", headers, new FileInputStream(f));
                }

                if(path.equals("/favicon.ico"))
                    return answer(404, "text/plain", new byte[0]);

                String query = u.getEncodedQuery();
                long timeout = path.equals("/strike") ? 12000 : 3000;     // LAST STRIKE: ~5 KB, paced when the watch is on too

                RodLink.Response r = RodLink.get(MainActivity.this).request(query == null ? path : path + "?" + query, timeout);

                if(r == null)
                    return answer(503, "text/plain", "NO CONNECTION".getBytes("UTF-8"));

                boolean json = r.body.length > 0 && (r.body[0] == '{' || r.body[0] == '[');

                return answer(r.code, json ? "application/json" : "text/plain", r.body);
            }
            catch(Exception e)
            {
                return answer(500, "text/plain", new byte[0]);
            }
        }

        @SuppressWarnings("deprecation")
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, String url)
        {
            // Anything that is not ours opens outside the app
            if(url.startsWith(DASHBOARD_URL) || url.startsWith("file:///android_asset/"))
                return false;

            try
            {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            }
            catch(Exception e)
            {
            }

            return true;
        }
    }

    private WebResourceResponse asset(String name, String type) throws Exception
    {
        InputStream in = getAssets().open(name);
        WebResourceResponse r = new WebResourceResponse(type, "UTF-8", in);
        r.setStatusCodeAndReasonPhrase(200, "OK");
        return r;
    }

    private static WebResourceResponse answer(int code, String type, byte[] body)
    {
        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store");

        String reason = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 503 ? "Unavailable" : "Error";

        return new WebResourceResponse(type, "UTF-8", code, reason, headers, new ByteArrayInputStream(body));
    }

    // ---------- lifecycle ----------

    @Override
    protected void onResume()
    {
        super.onResume();
        visible = true;
        RodLink.get(this).setFast(true);

        // The dashboard sounds its own alarm: quiet the background one
        Intent i = new Intent(this, RodService.class).setAction(RodService.ACTION_APP_VISIBLE);

        try
        {
            startService(i);
        }
        catch(Exception e)
        {
        }
    }

    @Override
    protected void onPause()
    {
        super.onPause();
        visible = false;
        RodLink.get(this).setFast(false);
    }

    @Override
    protected void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed()
    {
        if(tab == TAB_SETTINGS)
        {
            showTab(TAB_ROD);
            return;
        }

        if(strikes.getVisibility() == View.VISIBLE)
        {
            // the page goes back a step (strike -> list); on the list: close
            strikes.evaluateJavascript("window.appBack ? appBack() : false", new android.webkit.ValueCallback<String>()
            {
                @Override
                public void onReceiveValue(String r)
                {
                    if(!"true".equals(r))
                        closeStrikes();
                }
            });
            return;
        }

        moveTaskToBack(true);      // keep watching in the background
    }

    // ---------- MY STRIKES ----------

    @SuppressLint({ "SetJavaScriptEnabled", "AddJavascriptInterface" })
    private void makeStrikesView(FrameLayout root)
    {
        strikes = new WebView(this);
        strikes.setBackgroundColor(Color.rgb(3, 8, 20));
        strikes.setVisibility(View.GONE);

        WebSettings s = strikes.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setTextZoom(100);

        strikes.addJavascriptInterface(new StrikesBridge(), "StrikeStore");
        strikes.setWebChromeClient(new WebChromeClient());
        strikes.setWebViewClient(new Client());
        root.addView(strikes);
    }

    // ---------- bottom tabs: Rod / My strikes / Map / Settings ----------

    private int dp(int v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private View makeTabs()
    {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(Color.rgb(7, 12, 24));

        String[] icons = { "\u25C9", "\u2630", "\u2691", "\u2699" };
        String[] names = { "Rod", "My strikes", "Map", "Settings" };

        for(int i = 0; i < 4; i++)
        {
            final int which = i;
            TextView t = new TextView(this);
            t.setText(icons[i] + "\n" + names[i]);
            t.setGravity(Gravity.CENTER);
            t.setTextSize(12);
            t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            t.setOnClickListener(new View.OnClickListener()
            {
                @Override
                public void onClick(View v)
                {
                    showTab(which);
                }
            });
            tabs[i] = t;
            bar.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1));
        }

        lightTab(TAB_ROD);
        return bar;
    }

    private void lightTab(int which)
    {
        tab = which;

        for(int i = 0; i < 4; i++)
            tabs[i].setTextColor(i == which ? Color.rgb(34, 211, 255) : Color.rgb(95, 119, 153));
    }

    private void showTab(int which)
    {
        if(which == TAB_STRIKES || which == TAB_MAP)
        {
            String page = which == TAB_MAP ? "pMap" : "pList";

            if(strikes.getVisibility() != View.VISIBLE)
            {
                strikes.setVisibility(View.VISIBLE);
                strikes.loadUrl(DASHBOARD_URL + "app/strikes.html#" + page);
            }
            else
            {
                strikes.evaluateJavascript("show('" + page + "')", null);
            }

            web.evaluateJavascript("if(window.closeSettings) document.getElementById('drawer').classList.remove('open')", null);
        }
        else
        {
            strikes.setVisibility(View.GONE);
            strikes.loadUrl("about:blank");

            // Settings: the dashboard's settings panel; Rod: the live screen
            web.evaluateJavascript(which == TAB_SETTINGS
                ? "if(window.openSettings) openSettings()"
                : "var d=document.getElementById('drawer'); if(d) d.classList.remove('open')", null);
        }

        lightTab(which);
    }

    private void openStrikes()
    {
        showTab(TAB_STRIKES);
    }

    private void closeStrikes()
    {
        showTab(TAB_ROD);
    }

    // Called from the MY STRIKES page
    private class StrikesBridge
    {
        @JavascriptInterface
        public String list()
        {
            return StrikeStore.get(MainActivity.this).listJson();
        }

        @JavascriptInterface
        public String strike(String uid)
        {
            return StrikeStore.get(MainActivity.this).strikeJson(uid);
        }

        @JavascriptInterface
        public void delete(String uid)
        {
            StrikeStore.get(MainActivity.this).delete(uid);
        }

        // the strike's fight record (knock + tilt, strike to the end), "null" = none
        @JavascriptInterface
        public String fight(String uid)
        {
            return StrikeStore.get(MainActivity.this).record(uid, "fight");
        }

        @JavascriptInterface
        public boolean hasDetail(String uid)
        {
            return !"null".equals(StrikeStore.get(MainActivity.this).record(uid, "detail"));
        }

        // CATCH page
        @JavascriptInterface
        public String newestUid()
        {
            return StrikeStore.get(MainActivity.this).newestUid();
        }

        @JavascriptInterface
        public void setCatch(String uid, String fish, String kg, String cm, String result, String mine)
        {
            StrikeStore.get(MainActivity.this).setCatch(uid, fish, kg, cm, result, mine);

            new Thread(new Runnable()
            {
                @Override
                public void run()
                {
                    StrikeStore.get(MainActivity.this).syncNow(true);
                }
            }).start();
        }

        @JavascriptInterface
        public boolean hasPhoto(String uid, String slot)
        {
            return StrikeStore.get(MainActivity.this).photoFile(uid, slot).exists();
        }

        // profile picture: camera (selfie) or gallery
        @JavascriptInterface
        public void profilePhoto(final boolean camera)
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    pickProfile(camera);
                }
            });
        }

        @JavascriptInterface
        public boolean hasProfile()
        {
            return profileFile(MainActivity.this).exists();
        }

        // take the photo now with the camera (no gallery: a real photo)
        @JavascriptInterface
        public void photo(final String uid, final String slot)
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    takePhoto(uid, slot);
                }
            });
        }

        @JavascriptInterface
        public String cloudNote()
        {
            return StrikeStore.get(MainActivity.this).cloudNote();
        }

        @JavascriptInterface
        public String username()
        {
            return StrikeStore.get(MainActivity.this).username();
        }

        // send now / get everything back from the sheet: the page is told when done
        @JavascriptInterface
        public void sync(final boolean restore)
        {
            new Thread(new Runnable()
            {
                @Override
                public void run()
                {
                    StrikeStore st = StrikeStore.get(MainActivity.this);
                    int got = restore ? st.restore() : 0;
                    st.syncNow(true);
                    final String note = restore
                        ? (got >= 0 ? got + " strikes loaded from the sheet" : got == -1 ? "Google Sheet not set up" : "could not reach the sheet")
                        : st.cloudNote();

                    handler.post(new Runnable()
                    {
                        @Override
                        public void run()
                        {
                            strikes.evaluateJavascript("window.synced && synced(" + quote(note) + ")", null);
                        }
                    });
                }
            }).start();
        }

        // the page changed (map, list...): light the matching tab
        @JavascriptInterface
        public void pageShown(final String id)
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(strikes.getVisibility() == View.VISIBLE)
                        lightTab("pMap".equals(id) ? TAB_MAP : TAB_STRIKES);
                }
            });
        }

        @JavascriptInterface
        public void close()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    closeStrikes();
                }
            });
        }
    }

    @Override
    protected void onDestroy()
    {
        handler.removeCallbacksAndMessages(null);
        web.destroy();
        strikes.destroy();
        super.onDestroy();
    }

    // Called from the help page
    private class Bridge
    {
        @JavascriptInterface
        public void openBluetooth()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    RodLink.State s = RodLink.get(MainActivity.this).state();

                    if(s == RodLink.State.NO_PERMISSION)
                    {
                        if(askPermissions())
                            permissionsReady();

                        return;
                    }

                    askedBluetoothOn = false;
                    askBluetoothOn();

                    try
                    {
                        if(s != RodLink.State.BLUETOOTH_OFF)
                            startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS));
                    }
                    catch(Exception e)
                    {
                    }
                }
            });
        }

        @JavascriptInterface
        public void appSettings()
        {
            try
            {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                         Uri.parse("package:" + getPackageName())));
            }
            catch(Exception e)
            {
            }
        }

        // The dashboard's chosen alarm sound per alarm type (strike, slow,
        // knock, drop): RodService plays it when the app is in the background
        @JavascriptInterface
        public void setSound(String cat, String name)
        {
            if(cat == null || name == null || !RodService.SOUNDS.contains(name)
               || !(cat.equals("strike") || cat.equals("slow") || cat.equals("knock") || cat.equals("drop")))
                return;

            getSharedPreferences("sounds", MODE_PRIVATE).edit().putString(cat, name).apply();
        }

        @JavascriptInterface
        public void openStrikes()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    MainActivity.this.openStrikes();
                }
            });
        }

        // ---------- REGISTER / LOG IN page ----------

        // register = true: new account; false: log in. The page asks accountResult().
        @JavascriptInterface
        public void accountStart(final boolean register, final String user, final String password)
        {
            accountResult = "working";

            new Thread(new Runnable()
            {
                @Override
                public void run()
                {
                    StrikeStore st = StrikeStore.get(MainActivity.this);
                    String err = st.account(register, user == null ? "" : user, password == null ? "" : password);

                    if(err.isEmpty())
                    {
                        if(!register)
                            st.restore();          // a new phone: this tester's strikes come back
                        st.syncNow(true);
                    }

                    accountResult = err.isEmpty() ? "ok" : "error:" + err;
                }
            }).start();
        }

        @JavascriptInterface
        public String accountResult()
        {
            return accountResult;
        }

        // the page says OK (or "later"): go on to the rod
        @JavascriptInterface
        public void accountDone(boolean later)
        {
            if(later)
                getSharedPreferences("app", MODE_PRIVATE).edit().putBoolean("accountLater", true).apply();

            accountAsked = false;
        }

        // Settings > ACCOUNT > SIGN IN
        @JavascriptInterface
        public void openAccount()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    accountResult = "";
                    accountAsked = true;
                    showTab(TAB_ROD);
                }
            });
        }

        @JavascriptInterface
        public String accountName()
        {
            return StrikeStore.get(MainActivity.this).username();
        }

        @JavascriptInterface
        public void logOut()
        {
            StrikeStore.get(MainActivity.this).logOut();
            openAccount();
        }

        // ---------- PAIR YOUR ROD page ----------

        @JavascriptInterface
        public void pairScan(boolean on)
        {
            RodLink.get(MainActivity.this).setPairScan(on);
        }

        @JavascriptInterface
        public String pairList()
        {
            StringBuilder b = new StringBuilder("[");

            for(RodLink.RodSeen r : RodLink.get(MainActivity.this).pairList())
            {
                if(b.length() > 1) b.append(',');
                b.append("{\"id\":").append(org.json.JSONObject.quote(r.id))
                 .append(",\"address\":").append(org.json.JSONObject.quote(r.address))
                 .append(",\"rssi\":").append(r.rssi).append('}');
            }

            return b.append(']').toString();
        }

        @JavascriptInterface
        public void pair(String address, String id, String pin)
        {
            if(address != null && id != null && pin != null && pin.matches("[0-9]{6}"))
                RodLink.get(MainActivity.this).pair(address, id, pin);
        }

        @JavascriptInterface
        public String pairState()
        {
            RodLink link = RodLink.get(MainActivity.this);
            return link.pairState().name().toLowerCase(java.util.Locale.ROOT) + (link.ready() ? ",connected" : "");
        }

        @JavascriptInterface
        public String rodId()
        {
            return RodLink.get(MainActivity.this).rodId();
        }

        // Settings > MY ROD > Forget my devices
        @JavascriptInterface
        public void forgetRod()
        {
            new Thread(new Runnable()
            {
                @Override
                public void run()
                {
                    RodLink.get(MainActivity.this).forget();

                    // a fresh start with the rod: Phone + watch again (not watch only)
                    if(RodService.isWatchOnly(MainActivity.this))
                        setWatchOnly(false);

                    handler.post(new Runnable()
                    {
                        @Override
                        public void run()
                        {
                            onDashboard = false;
                            onPairPage = false;
                            showTab(TAB_ROD);
                        }
                    });
                }
            }).start();
        }

        // the settings panel was closed on the page: the Rod tab lights again
        @JavascriptInterface
        public void settingsClosed()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    if(tab == TAB_SETTINGS)
                        lightTab(TAB_ROD);
                }
            });
        }

        // Phone + watch (false) / Watch only (true)
        @JavascriptInterface
        public void setWatchOnly(boolean on)
        {
            Intent i = new Intent(MainActivity.this, RodService.class)
                .setAction(RodService.ACTION_WATCH_ONLY)
                .putExtra(RodService.EXTRA_ON, on);

            try
            {
                startService(i);
            }
            catch(Exception e)
            {
            }
        }

        // FISH OUT tapped (or ADD PHOTOS): the catch page
        @JavascriptInterface
        public void fishOut()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    openCatch();
                }
            });
        }

        @JavascriptInterface
        public void retry()
        {
            RodLink.get(MainActivity.this).kick();
        }

        @JavascriptInterface
        public void stopWatching()
        {
            Intent i = new Intent(MainActivity.this, RodService.class).setAction(RodService.ACTION_STOP);

            try
            {
                startService(i);
            }
            catch(Exception e)
            {
            }

            finish();
        }
    }
}
