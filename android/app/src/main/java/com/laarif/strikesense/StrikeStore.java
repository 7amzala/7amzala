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
    private final File photos;              // the catch photos: <uid>_<slot>.jpg
    private JSONArray list;                 // newest last, without graphs

    private Location lastFix;
    private long lastFixAsked = 0;
    private volatile boolean nearRod = false;  // connected to the rod now (the phone is within ~30 m of it)
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
        photos = new File(c.getFilesDir(), "photos");
        photos.mkdirs();

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
    synchronized void fromRod(JSONObject rod, long now, JSONObject sea, String rodId)
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
                put(s, "local", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.UK).format(new java.util.Date(start)));
                put(s, "rod", rodId == null ? "" : rodId);
                put(s, "graph", false);
                put(s, "synced", false);

                if(sea != null)
                {
                    put(s, "wave", sea.optInt("wave", -1));
                    put(s, "wind", sea.optInt("wind", -1));
                }

                addPlace(s, start, false);
                list.put(s);
                changed = true;
            }

            if(r.optLong("seq") > 0 && !s.has("rseq"))
            {
                put(s, "rseq", r.optLong("seq"));     // its number in the rod's strike log
                changed = true;
            }

            if(landing(s, r))
                changed = true;

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

    // The rod's strike log (/log): every strike it saved, also those made while
    // the phone was away or off. Returns the highest log number in it.
    synchronized long fromLog(JSONObject log, long now, String rodId)
    {
        JSONArray l = log.optJSONArray("list");
        long top = 0;

        if(l == null)
            return 0;

        boolean changed = false;
        String rod = rodId == null ? "" : rodId;

        for(int i = 0; i < l.length(); i++)
        {
            JSONObject r = l.optJSONObject(i);

            if(r == null)
                continue;

            long seq = r.optLong("seq");
            long utc = r.optLong("utc");
            long ago = r.optLong("ago", -1);
            String type = r.optString("type", "STRIKE");
            top = Math.max(top, seq);

            // its time: the rod's clock, else "so long ago" (same power-on)
            long start = utc > 0 ? utc * 1000 : ago >= 0 ? now - ago * 1000 : 0;

            JSONObject s = bySeq(rod, seq);

            if(s == null && start > 0)
                s = near(rod, type, start);      // got live before (older app or rod)

            if(s == null)
            {
                boolean noTime = start == 0;

                if(noTime)
                    start = now;                 // the rod did not know the time: when it came

                s = new JSONObject();
                put(s, "uid", Long.toString(start / 1000) + "-" + Math.abs(type.hashCode() % 1000));
                put(s, "time", start);
                put(s, "type", type);
                put(s, "local", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.UK).format(new java.util.Date(start)));
                put(s, "rod", rod);
                put(s, "graph", false);
                put(s, "synced", false);
                put(s, "later", true);           // saved on the rod while the phone was away

                if(noTime)
                    put(s, "notime", true);

                addPlace(s, start, true);
                list.put(s);
                changed = true;
            }

            if(!s.has("rseq"))
                put(s, "rseq", seq);

            if(!s.optBoolean("done") || s.optInt("fight", -1) != r.optInt("fight"))
            {
                put(s, "fight", r.optInt("fight"));
                put(s, "peak", r.optDouble("peak"));
                put(s, "knock", r.optDouble("knock"));
                put(s, "knocks", r.optInt("knocks"));
                put(s, "done", true);
                put(s, "synced", false);
                changed = true;
            }

            if(landing(s, r))
                changed = true;

            // the rod's strike record (raw sensor) and fight record: to collect
            if(r.optBoolean("detail") && !s.optBoolean("rdetail"))
            {
                put(s, "rdetail", true);
                changed = true;
            }

            if(r.optBoolean("fightrec") && !s.optBoolean("rfight"))
            {
                put(s, "rfight", true);
                changed = true;
            }

            if(!r.optBoolean("graph") && !s.optBoolean("graph") && !s.optBoolean("nograph"))
            {
                put(s, "nograph", true);         // the rod had no graph for it
                changed = true;
            }
        }

        if(changed)
        {
            sortByTime();
            save();
        }

        return top;
    }

    // REEL IN -> FISH OUT / LOST IT from the rod (watch or app). A result set
    // in the app (CATCH page) is not undone by a rod that has none.
    private static boolean landing(JSONObject s, JSONObject r)
    {
        String res = r.optString("result", "");
        int land = r.optInt("land", 0);

        if(res.isEmpty() || (res.equals(s.optString("result")) && land == s.optInt("land", -1)))
            return false;

        put(s, "result", res);
        put(s, "land", land);
        put(s, "synced", false);
        return true;
    }

    // ---------- the catch: fish, weight, length, 3 photos ----------

    static final String[] PHOTO_SLOTS = { "hook", "weight", "length" };

    File photoFile(String uid, String slot)
    {
        return new File(photos, uid.replaceAll("[^A-Za-z0-9_-]", "") + "_" + slot + ".jpg");
    }

    // The newest strike of the last 30 minutes (FISH OUT just tapped), else ""
    synchronized String newestUid()
    {
        JSONObject s = list.length() > 0 ? list.optJSONObject(list.length() - 1) : null;
        return s != null && System.currentTimeMillis() - s.optLong("time") < 30 * 60000L ? s.optString("uid") : "";
    }

    // From the CATCH page: what fish, kg, cm ("" = not given); marks it a fish
    synchronized void setCatch(String uid, String fish, String kg, String cm, String result, String mine)
    {
        JSONObject s = byUid(uid);

        if(s == null)
            return;

        put(s, "fish", fish.trim());
        put(s, "kg", kg.trim());
        put(s, "cm", cm.trim());
        put(s, "mine", mine.trim());             // the fisher's own name for this fish

        if(!result.isEmpty())
            put(s, "result", result);

        put(s, "synced", false);
        save();
    }

    // A photo was taken (camera only, already made small): goes online with the
    // next sync, with when and where it was taken (the sheet shows it)
    synchronized void photoSaved(String uid, String slot, long takenMs)
    {
        JSONObject s = byUid(uid);

        if(s == null)
            return;

        try
        {
            JSONObject ph = s.optJSONObject("photos");

            if(ph == null)
            {
                ph = new JSONObject();
                s.put("photos", ph);
            }

            ph.put(slot, "");                // "" = on the phone, not online yet

            JSONObject info = s.optJSONObject("photoinfo");

            if(info == null)
            {
                info = new JSONObject();
                s.put("photoinfo", info);
            }

            JSONObject one = new JSONObject();
            one.put("at", takenMs);
            one.put("local", new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.UK).format(new java.util.Date(takenMs)));
            Location l = bestFix();

            if(l != null && Math.abs(takenMs - l.getTime()) < 15 * 60000L)
            {
                one.put("lat", Math.round(l.getLatitude() * 1e5) / 1e5);
                one.put("lon", Math.round(l.getLongitude() * 1e5) / 1e5);
            }

            info.put(slot, one);
            if(s.optString("result").isEmpty())
                s.put("result", "fish");
            s.put("synced", false);
            save();
        }
        catch(Exception e)
        {
        }
    }

    // Photos on the phone not online yet: [uid, slot] pairs
    private synchronized List<String[]> photosToSend()
    {
        List<String[]> out = new java.util.ArrayList<>();

        for(int i = 0; i < list.length(); i++)
        {
            JSONObject s = list.optJSONObject(i);
            JSONObject ph = s == null ? null : s.optJSONObject("photos");

            if(ph == null)
                continue;

            for(String slot : PHOTO_SLOTS)
                if(ph.has(slot) && ph.optString(slot).isEmpty() && photoFile(s.optString("uid"), slot).exists())
                    out.add(new String[] { s.optString("uid"), slot });
        }

        return out;
    }

    // How far the app got in this rod's strike log
    long logSeq(String rodId)
    {
        return prefs().getLong("logseq_" + (rodId == null ? "" : rodId), 0);
    }

    void setLogSeq(String rodId, long seq)
    {
        prefs().edit().putLong("logseq_" + (rodId == null ? "" : rodId), seq).apply();
    }

    private JSONObject bySeq(String rod, long seq)
    {
        for(int i = list.length() - 1; i >= 0; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optLong("rseq") == seq && s.optString("rod").equals(rod))
                return s;
        }

        return null;
    }

    // the same strike already here without a log number: same rod and type, within 8 s
    private JSONObject near(String rod, String type, long start)
    {
        for(int i = list.length() - 1; i >= 0; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && !s.has("rseq") && s.optString("type").equals(type) && s.optString("rod").equals(rod)
               && Math.abs(s.optLong("time") - start) < 8000)
                return s;
        }

        return null;
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
        for(int i = list.length() - 1; i >= 0 && i >= list.length() - 40; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s != null && s.optBoolean("done") && !s.optBoolean("graph") && !s.optBoolean("nograph")
               && (s.has("rodid") || s.has("rseq")))
                return s;
        }

        return null;
    }

    // ---------- strike + fight records (for better alarms, real simulations) ----------

    // The newest strike whose strike or fight record is still on the rod only;
    // "what" is set to "detail" or "fight". null = none.
    synchronized JSONObject needsRecord(String[] what)
    {
        for(int i = list.length() - 1; i >= 0 && i >= list.length() - 60; i--)
        {
            JSONObject s = list.optJSONObject(i);

            if(s == null || !s.has("rseq"))
                continue;

            if(s.optBoolean("rdetail") && !s.optBoolean("hasdetail") && !s.optBoolean("nodetail"))
            {
                what[0] = "detail";
                return s;
            }

            if(s.optBoolean("rfight") && !s.optBoolean("hasfight") && !s.optBoolean("nofight"))
            {
                what[0] = "fight";
                return s;
            }
        }

        return null;
    }

    // kept next to the strike's graph (big: not in the list, not in the sheet)
    synchronized void setRecord(String uid, String what, String json)
    {
        JSONObject s = byUid(uid);

        if(s == null)
            return;

        try
        {
            if(json != null)
                write(new File(graphs, uid + "_" + what + ".json"), json);
        }
        catch(Exception e)
        {
            return;                         // (phone storage full: try again later)
        }

        put(s, json != null ? "has" + what : "no" + what, true);
        save();
    }

    synchronized String record(String uid, String what)
    {
        File f = new File(graphs, uid + "_" + what + ".json");

        try
        {
            return f.exists() ? read(f) : "null";
        }
        catch(Exception e)
        {
            return "null";
        }
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

    // Connected to the rod or not (RodService, every second or so)
    void setNearRod(boolean near)
    {
        nearRod = near;

        // near the rod with a fresh GPS fix: that is where the rod is
        Location l = near ? lastFix : null;

        if(l != null && System.currentTimeMillis() - l.getTime() < 120000)
            saveRodSpot(l);
    }

    // Where the rod stands: the phone's place while it was connected to it. The
    // rod does not move while you fish, so strikes saved while you were away
    // (car, cafe) get this place, not where the phone is when they arrive.
    private void saveRodSpot(Location l)
    {
        prefs().edit()
            .putLong("spotlat", Math.round(l.getLatitude() * 1e6))       // micro-degrees
            .putLong("spotlon", Math.round(l.getLongitude() * 1e6))
            .putLong("spotat", l.getTime())
            .apply();
    }

    private synchronized void gotFix(Location l)
    {
        if(lastFix == null || l.getTime() >= lastFix.getTime())
            lastFix = l;

        if(!nearRod)
            return;                     // away from the rod: not where the strikes were

        saveRodSpot(l);

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

    // later = saved on the rod while the phone was away: the rod's place
    private void addPlace(JSONObject s, long when, boolean later)
    {
        if(later)
        {
            SharedPreferences p = prefs();
            long at = p.getLong("spotat", 0);

            // the rod's place from the same fishing trip (within 12 h)
            if(at != 0 && Math.abs(at - when) < 12 * 3600000L)
            {
                put(s, "lat", Math.round(p.getLong("spotlat", 0) / 10.0) / 1e5);
                put(s, "lon", Math.round(p.getLong("spotlon", 0) / 10.0) / 1e5);
            }

            return;
        }

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

    // ---------- Google Sheet (the StrikeSense strikes database) ----------

    // Laarif StrikeSense's own sheet (Apps Script web app): every tester's
    // strikes go there with their username
    static final String SHEET_URL =
        "https://script.google.com/macros/s/AKfycbyecgNPh2ZRt9WnHZ2iRcKi4s7dBwNFlaneT3MwHEnEP76QVrh607H3pWrUPc36nXCt/exec";

    String cloudUrl()
    {
        return SHEET_URL;
    }

    // ---------- account: username + password (the sheet keeps only a check value) ----------

    String username()
    {
        return prefs().getString("user", "");
    }

    private String token()
    {
        return prefs().getString("token", "");
    }

    boolean signedIn()
    {
        return !username().isEmpty() && !token().isEmpty();
    }

    // register (true) or log in (false); returns "" when OK, else the reason
    // (network: never on the main thread)
    String account(boolean register, String user, String password)
    {
        try
        {
            JSONObject body = new JSONObject();
            body.put("action", register ? "register" : "login");
            body.put("username", user.trim());
            body.put("password", password);

            JSONObject answer = new JSONObject(http(SHEET_URL, body.toString()));

            if(!answer.optBoolean("ok"))
                return answer.optString("error", "Not possible right now");

            prefs().edit().putString("user", answer.optString("username", user.trim()))
                          .putString("token", answer.optString("token", "")).apply();
            lastCloudTry = 0;
            return "";
        }
        catch(Exception e)
        {
            return "No internet. Try again when the phone is online.";
        }
    }

    void logOut()
    {
        prefs().edit().remove("user").remove("token").apply();
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

        if(!signedIn())
        {
            cloudNote = "saved on this phone (sign in to save online)";
            return;
        }

        // the photos first (each on its own: ~300 KB): the row then gets their links
        int photoWait = 0;

        for(String[] p : photosToSend())
        {
            try
            {
                FileInputStream in = new FileInputStream(photoFile(p[0], p[1]));
                byte[] jpg;

                try
                {
                    jpg = readAll(in);
                }
                finally
                {
                    in.close();
                }

                JSONObject body = new JSONObject();
                body.put("action", "photo");
                body.put("username", username());
                body.put("token", token());
                body.put("uid", p[0]);
                body.put("slot", p[1]);

                synchronized(this)
                {
                    JSONObject s = byUid(p[0]);
                    JSONObject info = s == null || s.optJSONObject("photoinfo") == null ? null : s.optJSONObject("photoinfo").optJSONObject(p[1]);

                    if(info != null)
                        body.put("taken", info);
                }
                body.put("data", android.util.Base64.encodeToString(jpg, android.util.Base64.NO_WRAP));

                JSONObject answer = new JSONObject(http(url, body.toString()));

                if(!answer.optBoolean("ok") || answer.optString("url").isEmpty())
                    throw new Exception(answer.optString("error", "sheet said no"));

                synchronized(this)
                {
                    JSONObject s = byUid(p[0]);

                    if(s != null && s.optJSONObject("photos") != null)
                    {
                        s.optJSONObject("photos").put(p[1], answer.optString("url"));
                        s.put("synced", false);
                        todo = replaceInTodo(todo, s);
                        save();
                    }
                }
            }
            catch(Exception e)
            {
                photoWait++;
            }
        }

        if(todo.length() == 0)
        {
            cloudNote = photoWait > 0 ? photoWait + " photos waiting for internet" : "all saved online";
            return;
        }

        try
        {
            JSONObject body = new JSONObject();
            body.put("action", "add");
            body.put("username", username());
            body.put("token", token());
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

    // the strike (with its new photo link) in the list to send, added if missing
    private JSONArray replaceInTodo(JSONArray todo, JSONObject s)
    {
        JSONArray out = new JSONArray();
        String uid = s.optString("uid");

        for(int i = 0; i < todo.length(); i++)
        {
            JSONObject t = todo.optJSONObject(i);

            if(t != null && !t.optString("uid").equals(uid))
                out.put(t);
        }

        out.put(withGraph(s));
        return out;
    }

    // A new phone (or the app was reinstalled): get everything back from the sheet
    int restore()
    {
        String url = cloudUrl();

        if(!signedIn())
            return -1;

        try
        {
            JSONObject body = new JSONObject();
            body.put("action", "list");
            body.put("username", username());
            body.put("token", token());
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
                new File(graphs, uid + "_detail.json").delete();
                new File(graphs, uid + "_fight.json").delete();

                for(String slot : PHOTO_SLOTS)
                    photoFile(uid, slot).delete();
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
