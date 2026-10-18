package com.laarif.strikesense;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// Keeps the app on the rod's WiFi. The rod's WiFi has no internet, so
// Android would otherwise send the app's traffic over mobile data.
final class RodNetwork
{
    static final String ROD_URL = "http://192.168.4.1/";

    private static RodNetwork instance;

    private final ConnectivityManager cm;
    private volatile Network wifi;
    private boolean started = false;

    private RodNetwork(Context context)
    {
        cm = (ConnectivityManager) context.getApplicationContext().getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    static synchronized RodNetwork get(Context context)
    {
        if(instance == null)
            instance = new RodNetwork(context);

        return instance;
    }

    synchronized void start()
    {
        if(started)
            return;

        started = true;

        // WiFi, with or without internet (the rod has none)
        NetworkRequest request = new NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build();

        cm.requestNetwork(request, new ConnectivityManager.NetworkCallback()
        {
            @Override
            public void onAvailable(Network network)
            {
                wifi = network;
                bind(network);
            }

            @Override
            public void onLost(Network network)
            {
                if(network.equals(wifi))
                {
                    wifi = null;
                    bind(null);
                }
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void bind(Network network)
    {
        if(Build.VERSION.SDK_INT >= 23)
            cm.bindProcessToNetwork(network);
        else
            ConnectivityManager.setProcessDefaultNetwork(network);
    }

    // GET a page from the rod. Returns null if the rod does not answer.
    String fetch(String path, int timeoutMs)
    {
        HttpURLConnection c = null;

        try
        {
            URL url = new URL(ROD_URL + path);
            Network n = wifi;

            c = (HttpURLConnection) (n != null ? n.openConnection(url) : url.openConnection());
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setUseCaches(false);

            if(c.getResponseCode() != 200)
                return null;

            InputStream in = c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n2;

            while((n2 = in.read(buf)) > 0)
                out.write(buf, 0, n2);

            in.close();

            return out.toString("UTF-8");
        }
        catch(Exception e)
        {
            return null;
        }
        finally
        {
            if(c != null)
                c.disconnect();
        }
    }
}
