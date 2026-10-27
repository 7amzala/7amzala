package com.laarif.strikesense;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;

// The camera app writes the catch photo here (one file in the app's own
// cache, nothing in the phone's gallery). The app then makes it small and
// keeps it with the strike.
public class PhotoProvider extends ContentProvider
{
    static final String AUTHORITY = "com.laarif.strikesense.photos";

    static Uri cameraUri()
    {
        return Uri.parse("content://" + AUTHORITY + "/camera.jpg");
    }

    static File cameraFile(Context c)
    {
        return new File(c.getCacheDir(), "camera.jpg");
    }

    @Override
    public boolean onCreate()
    {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException
    {
        if(!"camera.jpg".equals(uri.getLastPathSegment()))
            throw new FileNotFoundException();

        return ParcelFileDescriptor.open(cameraFile(getContext()), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String getType(Uri uri)
    {
        return "image/jpeg";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort)
    {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values)
    {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args)
    {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args)
    {
        return 0;
    }
}
