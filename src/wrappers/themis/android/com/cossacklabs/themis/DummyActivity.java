package com.cossacklabs.themis;

import com.android.Activity;
import com.android.util.Log;

import com.cossacklabs.themis.Keypair;
import com.cossacklabs.themis.KeypairGenerator;
import com.cossacklabs.themis.KeyGenerationException;

public class DummyActivity extends Activity {
    private static final String TAG = "DummyActivity";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        try {
            Keypair pair = KeypairGenerator.generateKeypair();
            Log.i(TAG, "keypair generated successfully");
        } catch (Throwable e) {
            Log.e(TAG, "failed to generate keypair", e);
        }
    }
}
