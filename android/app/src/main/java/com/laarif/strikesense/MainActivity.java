package com.laarif.strikesense;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
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

    private static final int REQ_PERMISSIONS = 1;
    private static final int REQ_BLUETOOTH_ON = 2;

    static volatile boolean visible = false;

    private WebView web;
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
        setContentView(web);

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

        List<String> missing = new ArrayList<>();

        for(String p : need)
        {
            if(checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED)
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
        RodLink.get(this).kick();
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

            if(!onDashboard)
            {
                if(link.ready())
                {
                    onDashboard = true;
                    web.loadUrl(DASHBOARD_URL);
                }
                else
                {
                    js("if(window.setStatus) setStatus(" + quote(helpStatus(link.state())) + ")");
                }
            }
            else
            {
                // coloured signal bars (dashboard 2.3+); older page: the text
                js("if(window.setLink) setLink(" + link.ready() + "," + link.rssi() + ");"
                   + " else { var e=document.getElementById('linkInfo'); if(e) e.textContent=" + quote(linkText(link)) + "; }");
            }

            handler.postDelayed(this, 1000);
        }
    };

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

                if(path.equals("/favicon.ico"))
                    return answer(404, "text/plain", new byte[0]);

                String query = u.getEncodedQuery();
                long timeout = path.equals("/strike") ? 6000 : 3000;

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
        moveTaskToBack(true);      // keep watching in the background
    }

    @Override
    protected void onDestroy()
    {
        handler.removeCallbacksAndMessages(null);
        web.destroy();
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
