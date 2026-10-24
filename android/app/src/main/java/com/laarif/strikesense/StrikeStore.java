package com.laarif.strikesense;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.location.Address;
import android.location.Geocoder;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;

// The strikes the phone keeps (MY STRIKES).
//
// Each strike comes from the rod's list (/strikes: type, fight time,
// strongest pull), gets the phone's GPS place and the rod's graph
// (/laststrike), is saved on the phone first (works with no internet on
// the beach) and then sent to the user's own Google Sheet when there is
// internet. The sheet is only storage: everything is seen in the app.
final class StrikeStore
{
    private static StrikeStore instance;

    private final Context app;
    private final File index;
    private final File graphs;
    private JSONArray list;                 // newest last, without graphs

    private Location lastFix;
    private long lastFixAsked = 0;
    private long lastCloudTry = 0;
    private volatile String cloudNote = "";

    static synchronized StrikeStore get(Context c)
    {
        if(instance == null)
            instance = new StrikeStore(c.getApplicationContext());

        return instance;
    }

    private StrikeStore(Context c)
    {
        app = c;
        index = new File(c.getFilesDir(), "strikes.json");
        graphs = new File(c.getFilesDir(), "strike_graphs");
        graphs.mkdirs();

        try
        {
            list = new JSONArray(read(index));
        }
        catch(Exception e)
        {
            list = new JSONArray();
        }
    }

    // ---------- from the rod ----------

    // The rod's /strikes answer. now = phone time when it arrived;
    // sea = the last /data (waves, wind), may be null.
    synchronized void fromRod(JSONObject rod, long now, JSONObject sea)
    {
        JSONArray l = rod.optJSONArray("list");

        if(l == null)
            return;

        boolean changed = false;

        for(int i = 0; i < l.length(); i++)
        {
            JSONObject r = l.optJSONObject(i);

            if(r == null)
                continue;

            long start = now - r.optLong("ago") * 1000;
            String type = r.optString("type", "STRIKE");
            JSONObject s = find(type, start, r.optLong("id"));

            if(s == null)
            {
                s = new JSONObject();
                put(s, "uid", Long.toString(start / 1000) + "-" + Math.abs(type.hashCode() % 1000));
                put(s, "time", start);
                put(s, "type", type);
                put(s, "graph", false);
                put(s, "synced", false);

                if(sea != null)
                {
                    put(s, "wave", sea.optInt("wave", -1));
                    put(s, "wind", sea.optInt("wind", -1));
                }

                addPlace(s, start);
                list.put(s);
                changed = true;
            }

            // fight still going: the numbers grow
            if(s.optInt("fight", -1) != r.optInt("fight") || s.optBoolean("done") != r.optBoolean("done")
               || s.optDouble("peak", 0) != r.optDouble("peak"))
            {
                put(s, "rodid", r.optLong("id"));
                put(s, "fight", r.optInt("fight"));
                put(s, "peak", r.optDouble("peak"));
                put(s, "knock", r.optDouble("knock"));
                put(s, "knocks", r.optInt("knocks"));
                put(s, "done", r.optBoolean("done"));
                put(s, "synced", false);
                changed = true;
            }
        }

        if(changed)
            save();
    }

    // The same strike seen again: same rod number and type, start within 8 s
    // (the rod numbers start again from 1 when the rod restarts)
    private JSONObject find(String type, long start, long rodId)
    {
        for(int i = list.length() - 1; i >= 0 && i >= list.length() - 40; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optString("type").equals(type) && s.optLong("rodid", rodId) == rodId
               && Math.abs(s.optLong("time") - start) < 8000)
                return s;
        }

        return null;
    }

    // A fight still going on the rod: ask again soon
    synchronized boolean fightOpen()
    {
        JSONObject s = list.length() > 0 ? list.optJSONObject(list.length() - 1) : null;
        return s != null && !s.optBoolean("done");
    }

    // The newest finished strike still without its graph, with the rod id
    // it had (the rod's /laststrike "count"); null = none
    synchronized JSONObject needsGraph()
    {
        for(int i = list.length() - 1; i >= 0 && i >= list.length() - 3; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optBoolean("done") && !s.optBoolean("graph") && !s.optBoolean("nograph") && s.has("rodid"))
                return s;
        }

        return null;
    }

    // The rod's /laststrike for this strike (k/t 5 a second, zk/zt 50 a second)
    synchronized void setGraph(String uid, JSONObject g)
    {
        JSONObject s = byUid(uid);

        if(s == null)
            return;

        try
        {
            JSONObject keep = new JSONObject();
            keep.put("pre", g.optInt("pre"));
            keep.put("k", g.optJSONArray("k"));
            keep.put("t", g.optJSONArray("t"));
            keep.put("zpre", g.optInt("zpre"));
            keep.put("zk", g.optJSONArray("zk"));
            keep.put("zt", g.optJSONArray("zt"));
            keep.put("allow", g.optDouble("allow", 0));
            write(new File(graphs, uid + ".json"), keep.toString());

            put(s, "graph", true);
            put(s, "synced", false);
            save();
        }
        catch(Exception e)
        {
        }
    }

    // A newer strike was recorded over it on the rod (Watch only, two
    // strikes between checks): only the numbers are kept
    synchronized void noGraph(String uid)
    {
        JSONObject s = byUid(uid);

        if(s != null)
        {
            put(s, "nograph", true);
            save();
        }
    }

    // ---------- GPS place ----------

    private boolean canLocate()
    {
        return app.checkSelfPermission("android.permission.ACCESS_FINE_LOCATION") == PackageManager.PERMISSION_GRANTED
            || app.checkSelfPermission("android.permission.ACCESS_COARSE_LOCATION") == PackageManager.PERMISSION_GRANTED;
    }

    @SuppressLint("MissingPermission")
    private Location bestFix()
    {
        if(!canLocate())
            return null;

        LocationManager lm = (LocationManager) app.getSystemService(Context.LOCATION_SERVICE);

        if(lm == null)
            return lastFix;

        Location best = lastFix;

        for(String p : new String[] { LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER, "fused" })
        {
            try
            {
                Location l = lm.getLastKnownLocation(p);

                if(l != null && (best == null || l.getTime() > best.getTime()))
                    best = l;
            }
            catch(Exception e)
            {
            }
        }

        return best;
    }

    // Ask the GPS for a fresh fix now and then (fills in strikes without a place)
    @SuppressLint("MissingPermission")
    void refreshFix()
    {
        long now = System.currentTimeMillis();

        if(!canLocate() || now - lastFixAsked < 120000)
            return;

        lastFixAsked = now;
        LocationManager lm = (LocationManager) app.getSystemService(Context.LOCATION_SERVICE);

        if(lm == null)
            return;

        for(String p : new String[] { LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER })
        {
            try
            {
                if(!lm.isProviderEnabled(p))
                    continue;

                lm.requestSingleUpdate(p, new LocationListener()
                {
                    @Override public void onLocationChanged(Location l) { gotFix(l); }
                    @Override public void onStatusChanged(String s, int i, Bundle b) {}
                    @Override public void onProviderEnabled(String s) {}
                    @Override public void onProviderDisabled(String s) {}
                }, Looper.getMainLooper());
            }
            catch(Exception e)
            {
            }
        }
    }

    private synchronized void gotFix(Location l)
    {
        if(lastFix == null || l.getTime() >= lastFix.getTime())
            lastFix = l;

        // strikes of the last hour still without a place
        boolean changed = false;

        for(int i = 0; i < list.length(); i++)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && !s.has("lat") && Math.abs(s.optLong("time") - l.getTime()) < 3600000)
            {
                setLocation(s, l);
                changed = true;
            }
        }

        if(changed)
            save();
    }

    private void addPlace(JSONObject s, long when)
    {
        Location l = bestFix();

        // a fix from within 30 min of the strike: the fisherman has not moved far
        if(l != null && Math.abs(l.getTime() - when) < 1800000)
            setLocation(s, l);

        refreshFix();
    }

    private void setLocation(JSONObject s, Location l)
    {
        put(s, "lat", Math.round(l.getLatitude() * 1e5) / 1e5);
        put(s, "lon", Math.round(l.getLongitude() * 1e5) / 1e5);
        put(s, "synced", false);
    }

    // Country and beach / town names from the GPS position (needs internet:
    // done later when there is none at the strike)
    private boolean nameLocation(JSONObject s)
    {
        if(!s.has("lat") || s.has("country") || !Geocoder.isPresent())
            return false;

        try
        {
            Geocoder g = new Geocoder(app, Locale.ENGLISH);
            @SuppressWarnings("deprecation")
            List<Address> a = g.getFromLocation(s.optDouble("lat"), s.optDouble("lon"), 1);

            if(a == null || a.isEmpty())
                return false;

            Address ad = a.get(0);
            String place = ad.getLocality() != null ? ad.getLocality()
                         : ad.getSubAdminArea() != null ? ad.getSubAdminArea()
                         : ad.getAdminArea() != null ? ad.getAdminArea() : "";

            put(s, "country", ad.getCountryName() != null ? ad.getCountryName() : "?");
            put(s, "place", place);
            put(s, "synced", false);
            return true;
        }
        catch(Exception e)
        {
            return false;
        }
    }

    // ---------- Google Sheet ----------

    String cloudUrl()
    {
        return prefs().getString("cloudUrl", "");
    }

    void setCloudUrl(String url)
    {
        prefs().edit().putString("cloudUrl", url == null ? "" : url.trim()).apply();
        lastCloudTry = 0;
    }

    String cloudNote()
    {
        return cloudNote;
    }

    private SharedPreferences prefs()
    {
        return app.getSharedPreferences("strikes", Context.MODE_PRIVATE);
    }

    // From the background thread now and then: names places, sends what is new.
    // Never on the main thread (network).
    void syncNow(boolean force)
    {
        long now = System.currentTimeMillis();

        if(!force && now - lastCloudTry < 60000)
            return;

        lastCloudTry = now;

        JSONArray todo = new JSONArray();

        synchronized(this)
        {
            boolean named = false;

            for(int i = 0; i < list.length(); i++)
            {
                JSONObject s = list.optJSONObject(i);

                if(s != null && nameLocation(s))
                    named = true;
            }

            if(named)
                save();

            for(int i = 0; i < list.length(); i++)
            {
                JSONObject s = list.optJSONObject(i);

                if(s != null && !s.optBoolean("synced") && s.optBoolean("done"))
                    todo.put(withGraph(s));
            }
        }

        String url = cloudUrl();

        if(url.isEmpty())
        {
            cloudNote = "saved on this phone (Google Sheet not set up)";
            return;
        }

        if(todo.length() == 0)
        {
            cloudNote = "all saved online";
            return;
        }

        try
        {
            JSONObject body = new JSONObject();
            body.put("action", "add");
            body.put("strikes", todo);

            JSONObject answer = new JSONObject(http(url, body.toString()));

            if(!answer.optBoolean("ok"))
                throw new Exception(answer.optString("error", "sheet said no"));

            synchronized(this)
            {
                for(int i = 0; i < todo.length(); i++)
                {
                    JSONObject s = byUid(todo.getJSONObject(i).optString("uid"));

                    if(s != null)
                        put(s, "synced", true);
                }

                save();
            }

            cloudNote = "all saved online";
        }
        catch(Exception e)
        {
            cloudNote = todo.length() + " waiting for internet";
        }
    }

    // A new phone (or the app was reinstalled): get everything back from the sheet
    int restore()
    {
        String url = cloudUrl();

        if(url.isEmpty())
            return -1;

        try
        {
            JSONObject body = new JSONObject();
            body.put("action", "list");
            JSONObject answer = new JSONObject(http(url, body.toString()));
            JSONArray rows = answer.getJSONArray("strikes");
            int added = 0;

            synchronized(this)
            {
                for(int i = 0; i < rows.length(); i++)
                {
                    JSONObject r = rows.getJSONObject(i);
                    String uid = r.optString("uid");

                    if(uid.isEmpty() || byUid(uid) != null)
                        continue;

                    JSONObject g = r.optJSONObject("graphdata");
                    r.remove("graphdata");

                    if(g != null)
                        write(new File(graphs, uid + ".json"), g.toString());

                    put(r, "graph", g != null);
                    put(r, "synced", true);
                    list.put(r);
                    added++;
                }

                sortByTime();
                save();
            }

            return added;
        }
        catch(Exception e)
        {
            return -2;
        }
    }

    // POST to the Google Apps Script web app. Google answers with a
    // redirect to the result, which is read with a GET.
    private static String http(String url, String body) throws Exception
    {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(false);
        c.setDoOutput(true);
        c.setRequestMethod("POST");
        c.setRequestProperty("Content-Type", "text/plain; charset=utf-8");

        OutputStream out = c.getOutputStream();
        out.write(body.getBytes(StandardCharsets.UTF_8));
        out.close();

        int code = c.getResponseCode();

        for(int hops = 0; (code == 301 || code == 302 || code == 303 || code == 307) && hops < 5; hops++)
        {
            String next = c.getHeaderField("Location");
            c.disconnect();
            c = (HttpURLConnection) new URL(next).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            code = c.getResponseCode();
        }

        if(code != 200)
            throw new Exception("HTTP " + code);

        String text = new String(readAll(c.getInputStream()), StandardCharsets.UTF_8);
        c.disconnect();
        return text;
    }

    // ---------- for the MY STRIKES pages ----------

    synchronized String listJson()
    {
        return list.toString();
    }

    synchronized String strikeJson(String uid)
    {
        JSONObject s = byUid(uid);
        return s == null ? "null" : withGraph(s).toString();
    }

    synchronized void delete(String uid)
    {
        for(int i = 0; i < list.length(); i++)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optString("uid").equals(uid))
            {
                list.remove(i);
                new File(graphs, uid + ".json").delete();
                save();
                return;
            }
        }
    }

    private JSONObject withGraph(JSONObject s)
    {
        try
        {
            JSONObject full = new JSONObject(s.toString());
            File f = new File(graphs, s.optString("uid") + ".json");

            if(f.exists())
                full.put("graphdata", new JSONObject(read(f)));

            return full;
        }
        catch(Exception e)
        {
            return s;
        }
    }

    // ---------- files ----------

    private JSONObject byUid(String uid)
    {
        for(int i = 0; i < list.length(); i++)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optString("uid").equals(uid))
                return s;
        }

        return null;
    }

    private void sortByTime()
    {
        java.util.ArrayList<JSONObject> a = new java.util.ArrayList<>();

        for(int i = 0; i < list.length(); i++)
            a.add(list.optJSONObject(i));

        java.util.Collections.sort(a, new java.util.Comparator<JSONObject>()
        {
            @Override
            public int compare(JSONObject x, JSONObject y)
            {
                return Long.compare(x.optLong("time"), y.optLong("time"));
            }
        });

        list = new JSONArray(a);
    }

    private void save()
    {
        try
        {
            File tmp = new File(index.getPath() + ".tmp");
            write(tmp, list.toString());
            tmp.renameTo(index);
        }
        catch(Exception e)
        {
        }
    }

    private static void put(JSONObject o, String k, Object v)
    {
        try
        {
            o.put(k, v);
        }
        catch(Exception e)
        {
        }
    }

    private static String read(File f) throws Exception
    {
        InputStream in = new FileInputStream(f);
        String s = new String(readAll(in), StandardCharsets.UTF_8);
        in.close();
        return s;
    }

    private static void write(File f, String text) throws Exception
    {
        FileOutputStream out = new FileOutputStream(f);
        out.write(text.getBytes(StandardCharsets.UTF_8));
        out.close();
    }

    private static byte[] readAll(InputStream in) throws Exception
    {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];

        for(int n; (n = in.read(buf)) > 0; )
            b.write(buf, 0, n);

        return b.toByteArray();
    }
}
