package ph.edu.rsu.cajidiocan.smartclassroom;

import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.Key;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Stores the Firebase refresh token encrypted with an AES-GCM key that never leaves the
 * Android Keystore. A token is never written in plaintext.
 */
final class SecureTokenStore {
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "firebase_refresh_token_key";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final String PREF_ENCRYPTED = "refresh_token_enc";
    private static final String PREF_LEGACY_PLAINTEXT = "refresh_token";
    private static final int GCM_IV_BYTES = 12;
    private static final int GCM_TAG_BITS = 128;

    private final SharedPreferences prefs;

    SecureTokenStore(SharedPreferences prefs) {
        this.prefs = prefs;
    }

    /** @return the saved token, or "" when none exists or it can no longer be decrypted. */
    String load() {
        String legacy = prefs.getString(PREF_LEGACY_PLAINTEXT, "");
        if (!legacy.isEmpty()) {
            // Migrate a token saved in plaintext by an older app version.
            save(legacy);
            return legacy;
        }
        String stored = prefs.getString(PREF_ENCRYPTED, "");
        if (stored.isEmpty()) return "";
        try {
            byte[] blob = Base64.decode(stored, Base64.NO_WRAP);
            if (blob.length <= GCM_IV_BYTES) throw new IllegalStateException("Corrupt token");
            Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(GCM_TAG_BITS, blob, 0, GCM_IV_BYTES));
            byte[] plain = cipher.doFinal(blob, GCM_IV_BYTES, blob.length - GCM_IV_BYTES);
            return new String(plain, StandardCharsets.UTF_8);
        } catch (Exception e) {
            // Key invalidated (e.g. device restored) or data corrupted: the user signs in again.
            clear();
            return "";
        }
    }

    void save(String token) {
        try {
            Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.ENCRYPT_MODE, key());
            byte[] iv = cipher.getIV();
            if (iv.length != GCM_IV_BYTES) throw new IllegalStateException("Unexpected IV length");
            byte[] encrypted = cipher.doFinal(token.getBytes(StandardCharsets.UTF_8));
            byte[] blob = new byte[iv.length + encrypted.length];
            System.arraycopy(iv, 0, blob, 0, iv.length);
            System.arraycopy(encrypted, 0, blob, iv.length, encrypted.length);
            prefs.edit()
                    .putString(PREF_ENCRYPTED, Base64.encodeToString(blob, Base64.NO_WRAP))
                    .remove(PREF_LEGACY_PLAINTEXT)
                    .apply();
        } catch (Exception e) {
            // Never fall back to plaintext; the session just won't survive an app restart.
            clear();
        }
    }

    void clear() {
        prefs.edit().remove(PREF_ENCRYPTED).remove(PREF_LEGACY_PLAINTEXT).apply();
    }

    private static SecretKey key() throws Exception {
        KeyStore keyStore = KeyStore.getInstance(KEYSTORE);
        keyStore.load(null);
        Key existing = keyStore.getKey(KEY_ALIAS, null);
        if (existing instanceof SecretKey) return (SecretKey) existing;

        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build());
        return generator.generateKey();
    }
}
