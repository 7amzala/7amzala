package com.laarif.strikesense;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.location.Address;
import android.location.Geocoder;
import android.location.Location;
import android.os.Build;
import android.telephony.TelephonyManager;

import com.google.firebase.FirebaseNetworkException;
import com.google.firebase.FirebaseTooManyRequestsException;
import com.google.firebase.auth.AuthCredential;
import com.google.firebase.auth.AuthResult;
import com.google.firebase.auth.FirebaseAuth;
import com.google.firebase.auth.FirebaseAuthInvalidCredentialsException;
import com.google.firebase.auth.FirebaseAuthInvalidUserException;
import com.google.firebase.auth.FirebaseAuthUserCollisionException;
import com.google.firebase.auth.FirebaseAuthWeakPasswordException;
import com.google.firebase.auth.FirebaseUser;
import com.google.firebase.auth.PhoneAuthCredential;
import com.google.firebase.auth.PhoneAuthOptions;
import com.google.firebase.auth.PhoneAuthProvider;
import com.google.firebase.firestore.DocumentSnapshot;
import com.google.firebase.firestore.FieldValue;
import com.google.firebase.firestore.FirebaseFirestore;
import com.google.firebase.storage.FirebaseStorage;
import com.google.firebase.storage.StorageMetadata;
import com.google.firebase.storage.StorageReference;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.security.SecureRandom;
import java.util.Calendar;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.concurrent.TimeUnit;

// The StrikeSense account (Firebase project strikesense-acdcf).
//
// Register: the form -> email + password account -> SMS code (the phone is
// added to the account) -> email link -> the profile is saved. Both checks
// are needed. Log in: email + password, or phone + SMS code (same account).
//
// The page (app/account.html) calls the methods below and asks state() every
// few hundred ms: {step, busy, msg, err, ...}. Every Firebase call runs on its
// own (Tasks.await on a worker thread), never on the main thread.
//
//   users/{uid}     private: the whole form, the phone, the email, where and
//                   when the account was made. Only the owner and the admin.
//   profiles/{uid}  what other surfcasters see: name, city, country, avatar.
class Cloud
{
    private static Cloud one;

    static synchronized Cloud get(Context c)
    {
        if(one == null)
            one = new Cloud(c.getApplicationContext());
        return one;
    }

    private final Context app;
    private final FirebaseAuth auth;
    private Activity activity;              // the SMS check needs the screen (reCAPTCHA if Play Integrity says no)

    // what the page shows
    private volatile String step = "form";  // form | sms | email | done
    private volatile boolean busy = false;
    private volatile String msg = "", err = "";
    private volatile boolean login = false; // the SMS step of a log in (not of a new account)

    private String verificationId = "";
    private PhoneAuthProvider.ForceResendingToken resendToken;
    private String smsTo = "";
    private long smsSentAt = 0;

    private Cloud(Context c)
    {
        app = c;
        auth = FirebaseAuth.getInstance();

        // where the last start ended (a register can take days: the email link)
        FirebaseUser u = auth.getCurrentUser();
        step = u == null ? "form" : prefs().getString("step", "form");
    }

    void setActivity(Activity a)
    {
        activity = a;
    }

    private SharedPreferences prefs()
    {
        return app.getSharedPreferences("cloud", Context.MODE_PRIVATE);
    }

    // ---------- for the app ----------

    // a full account: both checks done and the profile saved
    boolean ready()
    {
        FirebaseUser u = auth.getCurrentUser();
        return u != null && "done".equals(step) && prefs().getBoolean("done", false);
    }

    String name()
    {
        return ready() ? prefs().getString("name", "") : "";
    }

    void logOut()
    {
        auth.signOut();
        prefs().edit().clear().apply();
        StrikeStore.get(app).logOut();
        step = "form";
        msg = err = "";
        login = false;
    }

    // ---------- the page asks this ----------

    String state()
    {
        try
        {
            JSONObject s = new JSONObject();
            FirebaseUser u = auth.getCurrentUser();

            s.put("step", step);
            s.put("busy", busy);
            s.put("msg", msg);
            s.put("err", err);
            s.put("login", login);
            s.put("smsTo", smsTo);
            s.put("codeSent", !verificationId.isEmpty());
            s.put("resendIn", Math.max(0, 60 - (System.currentTimeMillis() - smsSentAt) / 1000));
            s.put("email", u != null && u.getEmail() != null ? u.getEmail() : prefs().getString("email", ""));
            s.put("hasUser", u != null);
            s.put("hasForm", !prefs().getString("form", "").isEmpty());
            s.put("name", prefs().getString("name", ""));
            s.put("profile", new JSONObject(prefs().getString("profile", "{}")));
            return s.toString();
        }
        catch(Exception e)
        {
            return "{\"step\":\"form\"}";
        }
    }

    private void setStep(String s)
    {
        step = s;
        prefs().edit().putString("step", s).apply();
    }

    private void fail(String why)
    {
        err = why;
        msg = "";
        busy = false;
    }

    private boolean start(String working)
    {
        if(busy)
            return false;
        busy = true;
        err = "";
        msg = working;
        return true;
    }

    private interface Job { void run() throws Exception; }

    private void run(final Job j)
    {
        new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                try
                {
                    j.run();
                }
                catch(Exception e)
                {
                    fail(why(e));
                }
            }
        }).start();
    }

    private static <T> T await(Task<T> t) throws Exception
    {
        try
        {
            return Tasks.await(t, 60, TimeUnit.SECONDS);
        }
        catch(java.util.concurrent.ExecutionException e)
        {
            Throwable c = e.getCause();
            throw c instanceof Exception ? (Exception) c : e;
        }
    }

    // a short reason the fisherman understands
    private static String why(Exception e)
    {
        if(e instanceof FirebaseNetworkException || e instanceof java.util.concurrent.TimeoutException)
            return "No internet. Try again when the phone is online.";
        if(e instanceof FirebaseTooManyRequestsException)
            return "Too many tries. Wait a few minutes and try again.";
        if(e instanceof FirebaseAuthWeakPasswordException)
            return "Password too weak: at least 8 characters, letters and numbers.";
        if(e instanceof FirebaseAuthUserCollisionException)
        {
            String code = ((FirebaseAuthUserCollisionException) e).getErrorCode();
            if(code != null && code.contains("CREDENTIAL"))
                return "This phone number already has an account. Log in with it, or use another number.";
            return "This email already has an account. Tap LOG IN.";
        }
        if(e instanceof FirebaseAuthInvalidUserException)
            return "No account with this email. Tap REGISTER.";
        if(e instanceof FirebaseAuthInvalidCredentialsException)
        {
            String code = ((FirebaseAuthInvalidCredentialsException) e).getErrorCode();
            if(code != null && code.contains("VERIFICATION_CODE"))
                return "Wrong code. Check the SMS and try again.";
            if(code != null && code.contains("PHONE"))
                return "This phone number is not valid. Check the country code.";
            if(code != null && code.contains("EMAIL"))
                return "This email is not valid.";
            return "Wrong email or password.";
        }
        String m = e.getMessage();
        if(m != null && m.contains("region enabled"))
            return "SMS to this country is not switched on yet. The app owner has to allow it in Firebase (Authentication > Settings > SMS region policy).";
        if(m != null && (m.contains("BILLING") || m.contains("billing")))
            return "SMS needs the Firebase Blaze plan. The app owner has to switch it on.";
        return m == null || m.isEmpty() ? "Not possible right now. Try again." : m;
    }

    // ---------- the country: from where the phone is (the fisherman cannot choose it) ----------

    private volatile String countryJson = "";

    // {"iso":"GB","from":"gps"|"network"|"sim"|""}; "" while looking
    String country()
    {
        if(countryJson.isEmpty() && !lookingForCountry)
            findCountry();
        return countryJson;
    }

    private volatile boolean lookingForCountry = false;

    private void findCountry()
    {
        lookingForCountry = true;
        new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                String iso = "", from = "";
                StrikeStore st = StrikeStore.get(app);
                st.refreshFix();

                // the phone's own place (GPS or network): wait up to 20 s for a fix
                for(int i = 0; i < 20 && iso.isEmpty(); i++)
                {
                    Location l = st.bestFix();

                    if(l != null && Geocoder.isPresent())
                    {
                        try
                        {
                            List<Address> a = new Geocoder(app, Locale.ENGLISH).getFromLocation(l.getLatitude(), l.getLongitude(), 1);
                            if(a != null && !a.isEmpty() && a.get(0).getCountryCode() != null)
                            {
                                iso = a.get(0).getCountryCode().toUpperCase(Locale.ROOT);
                                from = "gps";
                                city = a.get(0).getLocality() != null ? a.get(0).getLocality() : "";
                            }
                        }
                        catch(Exception e)
                        {
                        }
                    }

                    if(iso.isEmpty())
                        try { Thread.sleep(1000); } catch(InterruptedException e) { }
                }

                // no place (location off, no map service): the mobile network the phone is on
                if(iso.isEmpty())
                {
                    TelephonyManager tm = (TelephonyManager) app.getSystemService(Context.TELEPHONY_SERVICE);
                    if(tm != null)
                    {
                        String n = tm.getNetworkCountryIso();
                        if(n != null && n.length() == 2) { iso = n.toUpperCase(Locale.ROOT); from = "network"; }
                        else
                        {
                            n = tm.getSimCountryIso();
                            if(n != null && n.length() == 2) { iso = n.toUpperCase(Locale.ROOT); from = "sim"; }
                        }
                    }
                }

                try
                {
                    JSONObject c = new JSONObject();
                    c.put("iso", iso);
                    c.put("from", from);
                    c.put("city", city);
                    countryJson = c.toString();
                }
                catch(Exception e)
                {
                }

                lookingForCountry = false;
            }
        }).start();
    }

    private volatile String city = "";

    // ---------- register ----------

    // form: {first,last,nick,city,dob:"YYYY-MM-DD",sex:"m"|"f",phone:"+44...",email,licence,country}
    // password apart (never saved). finish = true: the account exists already (an
    // earlier start, or a log in of an unfinished account): only the missing steps.
    void register(final String formJson, final String password, final boolean finish)
    {
        if(!start("Creating your account…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                JSONObject f = new JSONObject(formJson);
                String bad = check(f);
                if(!bad.isEmpty()) { fail(bad); return; }

                // the country is where the phone is: never what the page says
                JSONObject c = new JSONObject(countryJson.isEmpty() ? "{}" : countryJson);
                String iso = c.optString("iso", "");
                if(iso.isEmpty()) { fail("Turn on Location so StrikeSense can find your country, then try again."); return; }
                f.put("country", iso);
                f.put("countryFrom", c.optString("from"));

                prefs().edit().putString("form", f.toString()).putString("email", f.optString("email")).apply();

                FirebaseUser u = auth.getCurrentUser();

                if(u == null || !finish)
                {
                    if(u != null)
                        auth.signOut();
                    await(auth.createUserWithEmailAndPassword(f.optString("email").trim(), password));
                    u = auth.getCurrentUser();
                }

                login = false;
                next(u);
            }
        });
    }

    // what is still missing on this account: the phone, the email link, the profile
    private void next(FirebaseUser u) throws Exception
    {
        if(u == null) { setStep("form"); fail("Please register again."); return; }

        if(u.getPhoneNumber() == null || u.getPhoneNumber().isEmpty())
        {
            final JSONObject f = new JSONObject(prefs().getString("form", "{}"));
            setStep("sms");
            if(activity == null) { fail("Open StrikeSense and try again."); return; }
            activity.runOnUiThread(new Runnable() { @Override public void run() { sendSms(f.optString("phone")); } });
            return;
        }

        await(u.reload());
        u = auth.getCurrentUser();

        if(u != null && !u.isEmailVerified())
        {
            if(!prefs().getBoolean("emailSent", false))
            {
                await(u.sendEmailVerification());
                prefs().edit().putBoolean("emailSent", true).apply();
            }
            setStep("email");
            busy = false;
            msg = "";
            return;
        }

        saveProfile(u);
    }

    private static String check(JSONObject f)
    {
        if(f.optString("first").trim().isEmpty() || f.optString("last").trim().isEmpty())
            return "Please write your first and last name.";
        if(f.optString("city").trim().isEmpty())
            return "Please write your city.";
        if(!f.optString("sex").equals("m") && !f.optString("sex").equals("f"))
            return "Please choose Male or Female.";
        if(!f.optString("phone").matches("\\+[1-9][0-9]{6,14}"))
            return "Please check your phone number.";
        if(!f.optString("email").trim().matches("[^@\\s]+@[^@\\s]+\\.[^@\\s]+"))
            return "Please check your email.";
        if(age(f.optString("dob")) < 13)
            return "You must be 13 or older to make an account.";
        if(!f.optBoolean("agree"))
            return "Please tick the box to agree.";
        return "";
    }

    static int age(String dob)
    {
        try
        {
            String[] p = dob.split("-");
            Calendar now = Calendar.getInstance();
            int a = now.get(Calendar.YEAR) - Integer.parseInt(p[0]);
            int m = now.get(Calendar.MONTH) + 1, d = now.get(Calendar.DAY_OF_MONTH);
            int bm = Integer.parseInt(p[1]), bd = Integer.parseInt(p[2]);
            if(m < bm || (m == bm && d < bd))
                a--;
            return a;
        }
        catch(Exception e)
        {
            return -1;
        }
    }

    // ---------- the SMS code ----------

    private void sendSms(final String phone)
    {
        if(activity == null) { fail("Open StrikeSense and try again."); return; }

        smsTo = phone;
        verificationId = "";
        msg = "Sending the SMS…";
        busy = true;

        PhoneAuthOptions.Builder b = PhoneAuthOptions.newBuilder(auth)
            .setPhoneNumber(phone)
            .setTimeout(60L, TimeUnit.SECONDS)
            .setActivity(activity)
            .setCallbacks(new PhoneAuthProvider.OnVerificationStateChangedCallbacks()
            {
                @Override
                public void onCodeSent(String id, PhoneAuthProvider.ForceResendingToken token)
                {
                    verificationId = id;
                    resendToken = token;
                    smsSentAt = System.currentTimeMillis();
                    busy = false;
                    msg = "";
                }

                // Android read the SMS by itself (or the number was checked without one)
                @Override
                public void onVerificationCompleted(final PhoneAuthCredential cred)
                {
                    busy = true;
                    msg = "Phone confirmed…";
                    run(new Job() { @Override public void run() throws Exception { useCode(cred); } });
                }

                @Override
                public void onVerificationFailed(com.google.firebase.FirebaseException e)
                {
                    fail(why(e));
                }
            });

        if(resendToken != null)
            b.setForceResendingToken(resendToken);

        PhoneAuthProvider.verifyPhoneNumber(b.build());
    }

    // the page: the 6 digits from the SMS
    void smsCode(final String code)
    {
        if(verificationId.isEmpty()) { fail("Tap \"Send again\" for a new code."); return; }
        if(!start("Checking the code…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                useCode(PhoneAuthProvider.getCredential(verificationId, code.trim()));
            }
        });
    }

    void smsAgain(final String phone)
    {
        if(busy)
            return;
        err = "";

        if(phone != null && !phone.isEmpty() && !phone.equals(smsTo))
        {
            if(!phone.matches("\\+[1-9][0-9]{6,14}")) { fail("Please check your phone number."); return; }
            resendToken = null;                         // a new number: a new start
            try
            {
                JSONObject f = new JSONObject(prefs().getString("form", "{}"));
                f.put("phone", phone);
                prefs().edit().putString("form", f.toString()).apply();
            }
            catch(Exception e)
            {
            }
        }

        final String to = phone == null || phone.isEmpty() ? smsTo : phone;
        activity.runOnUiThread(new Runnable() { @Override public void run() { sendSms(to); } });
    }

    private void useCode(AuthCredential cred) throws Exception
    {
        if(login)
        {
            AuthResult r = await(auth.signInWithCredential(cred));

            // a phone with no account: Firebase just made an empty one. Remove it.
            if(r.getAdditionalUserInfo() != null && r.getAdditionalUserInfo().isNewUser())
            {
                try { await(r.getUser().delete()); } catch(Exception e) { auth.signOut(); }
                setStep("form");
                fail("No account with this phone number. Tap REGISTER.");
                return;
            }

            afterLogin();
            return;
        }

        FirebaseUser u = auth.getCurrentUser();
        await(u.linkWithCredential(cred));
        next(auth.getCurrentUser());
    }

    // "Back" on the SMS page: a log in just stops; a new account not finished is removed
    // (made a minute ago), so the form can be sent again
    void cancel()
    {
        if(busy)
            return;
        err = msg = "";
        verificationId = "";
        resendToken = null;

        final boolean wasLogin = login;
        login = false;
        setStep("form");

        if(wasLogin)
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                FirebaseUser u = auth.getCurrentUser();
                if(u != null && !prefs().getBoolean("done", false))
                {
                    try { await(u.delete()); } catch(Exception e) { auth.signOut(); }
                }
                prefs().edit().remove("emailSent").apply();
            }
        });
    }

    // ---------- the email link ----------

    // the page: "I tapped the link" (also asked by itself every few seconds)
    void emailCheck(final boolean quiet)
    {
        if(busy)
            return;
        busy = true;
        if(!quiet)
        {
            err = "";
            msg = "Checking…";
        }

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                FirebaseUser u = auth.getCurrentUser();
                if(u == null) { setStep("form"); fail("Please log in again."); return; }
                await(u.reload());
                u = auth.getCurrentUser();

                if(u == null || !u.isEmailVerified())
                {
                    busy = false;
                    msg = "";
                    if(!quiet)
                        err = "Not yet: open the email and tap the link first.";
                    return;
                }

                saveProfile(u);
            }
        });
    }

    void emailAgain()
    {
        if(!start("Sending the email…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                await(auth.getCurrentUser().sendEmailVerification());
                busy = false;
                msg = "Sent again. Look in Spam / Junk too.";
            }
        });
    }

    // a typo in the email: the link goes to the new one (it becomes the email once tapped)
    void emailChange(final String email)
    {
        if(!email.trim().matches("[^@\\s]+@[^@\\s]+\\.[^@\\s]+")) { fail("Please check your email."); return; }
        if(!start("Sending the email…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                await(auth.getCurrentUser().verifyBeforeUpdateEmail(email.trim()));
                prefs().edit().putString("email", email.trim()).apply();
                busy = false;
                msg = "Sent to " + email.trim() + ". Tap the link in it.";
            }
        });
    }

    // ---------- the profile (both checks done) ----------

    private void saveProfile(FirebaseUser u) throws Exception
    {
        msg = "Saving your profile…";
        await(u.getIdToken(true));                 // the new token says: phone + email checked

        FirebaseFirestore db = FirebaseFirestore.getInstance();
        String uid = u.getUid();
        DocumentSnapshot have = await(db.collection("users").document(uid).get());

        if(have.exists())
        {
            // made earlier (another phone): just use it
            afterLogin();
            return;
        }

        JSONObject f = new JSONObject(prefs().getString("form", "{}"));
        if(f.length() == 0) { setStep("form"); fail("Please fill in the form once more."); return; }

        StrikeStore st = StrikeStore.get(app);
        Location l = st.bestFix();

        Map<String, Object> reg = new HashMap<>();
        reg.put("at", FieldValue.serverTimestamp());
        reg.put("phoneTime", System.currentTimeMillis());
        reg.put("timeZone", TimeZone.getDefault().getID());
        if(l != null)
        {
            reg.put("lat", l.getLatitude());
            reg.put("lon", l.getLongitude());
            reg.put("accuracyM", Math.round(l.getAccuracy()));
        }
        reg.put("countryFrom", f.optString("countryFrom"));
        reg.put("device", Build.MANUFACTURER + " " + Build.MODEL + " (Android " + Build.VERSION.RELEASE + ")");
        reg.put("app", appVersion());

        // the old Google Sheet keeps getting the strikes, under an account made for this one
        String sheetUser = ("fb_" + uid).substring(0, Math.min(20, uid.length() + 3));
        String sheetPass = randomPassword();

        Map<String, Object> me = new HashMap<>();
        me.put("first", f.optString("first").trim());
        me.put("last", f.optString("last").trim());
        me.put("nick", f.optString("nick").trim());
        me.put("country", f.optString("country"));
        me.put("city", f.optString("city").trim());
        me.put("dob", f.optString("dob"));
        me.put("sex", f.optString("sex"));
        me.put("phone", u.getPhoneNumber());
        me.put("email", u.getEmail());
        me.put("licence", f.optString("licence").trim());
        me.put("agree", true);
        me.put("style", "surfcasting");
        me.put("reg", reg);
        me.put("createdAt", FieldValue.serverTimestamp());
        me.put("sheetUser", sheetUser);
        me.put("sheetPass", sheetPass);
        await(db.collection("users").document(uid).set(me));

        String photo = uploadPhoto(uid);

        Map<String, Object> pub = new HashMap<>();
        pub.put("name", f.optString("nick").trim().isEmpty() ? f.optString("first").trim() : f.optString("nick").trim());
        pub.put("first", f.optString("first").trim());
        pub.put("city", f.optString("city").trim());
        pub.put("country", f.optString("country"));
        pub.put("sex", f.optString("sex"));
        pub.put("photo", photo);
        pub.put("licensed", !f.optString("licence").trim().isEmpty());
        pub.put("since", FieldValue.serverTimestamp());
        await(db.collection("profiles").document(uid).set(pub));

        // the Sheet account (strikes keep going there until Firebase takes them)
        if(!st.account(true, sheetUser, sheetPass).isEmpty())
            st.account(false, sheetUser, sheetPass);
        st.syncNow(true);

        remember(me, pub, l);
    }

    private String uploadPhoto(String uid)
    {
        try
        {
            File f = MainActivity.profileFile(app);
            if(!f.exists())
                return "";

            byte[] b = new byte[(int) f.length()];
            FileInputStream in = new FileInputStream(f);
            int n = 0;
            while(n < b.length)
            {
                int r = in.read(b, n, b.length - n);
                if(r < 0)
                    break;
                n += r;
            }
            in.close();

            StorageReference ref = FirebaseStorage.getInstance().getReference("profiles/" + uid + "/photo.jpg");
            await(ref.putBytes(b, new StorageMetadata.Builder().setContentType("image/jpeg").build()));
            return String.valueOf(await(ref.getDownloadUrl()));
        }
        catch(Exception e)
        {
            return "";                                  // the profile works without it (default avatar)
        }
    }

    private String appVersion()
    {
        try
        {
            return app.getPackageManager().getPackageInfo(app.getPackageName(), 0).versionName;
        }
        catch(Exception e)
        {
            return "";
        }
    }

    private static String randomPassword()
    {
        SecureRandom r = new SecureRandom();
        String abc = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        StringBuilder b = new StringBuilder();
        for(int i = 0; i < 24; i++)
            b.append(abc.charAt(r.nextInt(abc.length())));
        return b.toString();
    }

    // the done page + Settings show this (kept on the phone)
    private void remember(Map<String, Object> me, Map<String, Object> pub, Location l) throws Exception
    {
        JSONObject p = new JSONObject();
        p.put("name", pub.get("name") != null ? pub.get("name") : me.get("first"));
        p.put("first", me.get("first"));
        p.put("last", me.get("last"));
        p.put("city", me.get("city"));
        p.put("country", me.get("country"));
        p.put("sex", me.get("sex"));
        p.put("phone", me.get("phone"));
        p.put("email", me.get("email"));
        p.put("licensed", pub.get("licensed"));
        p.put("photo", pub.get("photo"));
        p.put("at", System.currentTimeMillis());
        p.put("tz", TimeZone.getDefault().getID());
        if(l != null)
        {
            p.put("lat", l.getLatitude());
            p.put("lon", l.getLongitude());
        }

        prefs().edit()
            .putString("profile", p.toString())
            .putString("name", p.optString("name"))
            .putBoolean("done", true)
            .remove("form")
            .apply();

        setStep("done");
        busy = false;
        msg = "";
    }

    // ---------- log in ----------

    void loginEmail(final String email, final String password)
    {
        if(!start("Logging in…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                await(auth.signInWithEmailAndPassword(email.trim(), password));
                afterLogin();
            }
        });
    }

    void loginPhone(final String phone)
    {
        if(!phone.matches("\\+[1-9][0-9]{6,14}")) { fail("Please check your phone number."); return; }
        if(busy)
            return;
        err = "";
        login = true;
        resendToken = null;
        setStep("sms");
        activity.runOnUiThread(new Runnable() { @Override public void run() { sendSms(phone); } });
    }

    void forgotPassword(final String email)
    {
        if(!email.trim().matches("[^@\\s]+@[^@\\s]+\\.[^@\\s]+")) { fail("Write your email first, then tap the link."); return; }
        if(!start("Sending…"))
            return;

        run(new Job()
        {
            @Override
            public void run() throws Exception
            {
                await(auth.sendPasswordResetEmail(email.trim()));
                busy = false;
                msg = "Email sent to " + email.trim() + ". Tap the link in it to choose a new password.";
            }
        });
    }

    // signed in: a finished account comes back; an unfinished one goes on where it stopped
    private void afterLogin() throws Exception
    {
        login = false;
        FirebaseUser u = auth.getCurrentUser();
        await(u.reload());
        u = auth.getCurrentUser();

        FirebaseFirestore db = FirebaseFirestore.getInstance();
        DocumentSnapshot me = null;

        if(u.getPhoneNumber() != null && u.isEmailVerified())
        {
            await(u.getIdToken(true));
            me = await(db.collection("users").document(u.getUid()).get());
        }

        if(me == null || !me.exists())
        {
            // not finished: the missing step (the form again if this phone does not have it)
            if(prefs().getString("form", "").isEmpty())
            {
                setStep("form");
                busy = false;
                msg = "";
                err = "Your account is not finished. Fill in the form once more (your email and password stay).";
                prefs().edit().putBoolean("finish", true).apply();
                return;
            }
            next(u);
            return;
        }

        DocumentSnapshot pub = await(db.collection("profiles").document(u.getUid()).get());

        Map<String, Object> mine = me.getData() != null ? me.getData() : new HashMap<String, Object>();
        Map<String, Object> pubs = pub.exists() && pub.getData() != null ? pub.getData() : new HashMap<String, Object>();

        // the strikes of this account come back from the Sheet
        StrikeStore st = StrikeStore.get(app);
        String su = String.valueOf(mine.get("sheetUser")), sp = String.valueOf(mine.get("sheetPass"));
        if(mine.get("sheetUser") != null && st.account(false, su, sp).isEmpty())
            st.restore();
        st.syncNow(true);

        @SuppressWarnings("unchecked")
        Map<String, Object> reg = mine.get("reg") instanceof Map ? (Map<String, Object>) mine.get("reg") : null;
        Location l = null;
        if(reg != null && reg.get("lat") instanceof Number)
        {
            l = new Location("reg");
            l.setLatitude(((Number) reg.get("lat")).doubleValue());
            l.setLongitude(((Number) reg.get("lon")).doubleValue());
        }

        remember(mine, pubs, l);
    }
}
