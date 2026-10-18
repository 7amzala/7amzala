package com.laarif.strikesense;

import android.annotation.SuppressLint;
import android.app.Activity;
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
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

// Full screen dashboard from the rod (http://192.168.4.1). When the rod
// can't be reached, a help page is shown and the app keeps looking.
public class MainActivity extends Activity
{
    private static final String OFFLINE_URL = "file:///android_asset/offline.html";
    private static final int RETRY_MS = 3000;

    static volatile boolean visible = false;

    private WebView web;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean offline = false;
    private boolean checking = false;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle state)
    {
        super.onCreate(state);

        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        window.setStatusBarColor(Color.rgb(3, 8, 20));
        window.setNavigationBarColor(Color.rgb(3, 8, 20));

        RodNetwork.get(this).start();

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
        web.setWebViewClient(new WebViewClient()
        {
            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error)
            {
                if(request.isForMainFrame())
                    showOffline();
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onReceivedError(WebView view, int code, String description, String url)
            {
                if(Build.VERSION.SDK_INT < 23)
                    showOffline();
            }

            @SuppressWarnings("deprecation")
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url)
            {
                // Anything that is not the rod opens outside the app
                if(url.startsWith(RodNetwork.ROD_URL) || url.startsWith("file:///android_asset/"))
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
        });

        askPermissions();
        startRodService();

        showOffline();          // help page until the rod answers (checked at once)
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

    private void askPermissions()
    {
        if(Build.VERSION.SDK_INT >= 33 &&
           checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED)
        {
            requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 1);
        }

        // Once: let the app keep watching the rod with the screen off
        SharedPreferences prefs = getSharedPreferences("app", MODE_PRIVATE);
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);

        if(!prefs.getBoolean("askedBattery", false) && pm != null &&
           !pm.isIgnoringBatteryOptimizations(getPackageName()))
        {
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
    }

    private void showOffline()
    {
        if(!offline)
        {
            offline = true;
            web.loadUrl(OFFLINE_URL);
        }

        checkRod(0);
    }

    // Look for the rod; open the dashboard as soon as it answers
    private void checkRod(int delayMs)
    {
        if(checking)
            return;

        checking = true;

        handler.postDelayed(new Runnable()
        {
            @Override
            public void run()
            {
                new Thread(new Runnable()
                {
                    @Override
                    public void run()
                    {
                        final boolean found = RodNetwork.get(MainActivity.this).fetch("data", 2500) != null;

                        handler.post(new Runnable()
                        {
                            @Override
                            public void run()
                            {
                                checking = false;

                                if(!offline)
                                    return;

                                if(found)
                                {
                                    offline = false;
                                    web.loadUrl(RodNetwork.ROD_URL);
                                }
                                else
                                {
                                    checkRod(RETRY_MS);
                                }
                            }
                        });
                    }
                }).start();
            }
        }, delayMs);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        visible = true;

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
        if(web.canGoBack() && !offline)
            web.goBack();
        else
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
        public void openWifi()
        {
            try
            {
                startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
            }
            catch(Exception e)
            {
            }
        }

        @JavascriptInterface
        public void retry()
        {
            handler.post(new Runnable()
            {
                @Override
                public void run()
                {
                    checkRod(0);
                }
            });
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
