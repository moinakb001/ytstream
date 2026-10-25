package com.ytstream;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.google.android.gms.auth.api.identity.AuthorizationRequest;
import com.google.android.gms.auth.api.identity.AuthorizationResult;
import com.google.android.gms.auth.api.identity.Identity;
import com.google.android.gms.common.api.Scope;

import java.util.Collections;

/**
 * Obtains a YouTube OAuth access token through Google Play services.
 *
 * Requires an "Android" OAuth client in a Google Cloud project with the YouTube Data API v3
 * enabled, registered with this app's package name and signing-certificate SHA-1 (see README).
 */
final class YouTubeAuth {
    static final String SCOPE = "https://www.googleapis.com/auth/youtube";
    private static final String TAG = "YouTubeAuth";

    interface Callback {
        void onToken(String token);
        void onError(String message);
        /** Consent UI is needed; launch it with Activity.startIntentSenderForResult. */
        void onNeedsConsent(PendingIntent intent);
    }

    private YouTubeAuth() {}

    private static AuthorizationRequest request() {
        return AuthorizationRequest.builder()
                .setRequestedScopes(Collections.singletonList(new Scope(SCOPE)))
                .build();
    }

    static void authorize(Context ctx, Callback cb) {
        Identity.getAuthorizationClient(ctx)
                .authorize(request())
                .addOnSuccessListener(r -> {
                    if (r.hasResolution()) {
                        cb.onNeedsConsent(r.getPendingIntent());
                    } else if (r.getAccessToken() != null) {
                        cb.onToken(r.getAccessToken());
                    } else {
                        cb.onError("No access token returned");
                    }
                })
                .addOnFailureListener(e -> {
                    Log.w(TAG, "authorize failed", e);
                    cb.onError(e.getMessage() != null ? e.getMessage() : e.toString());
                });
    }

    /** Parse the result of the consent activity. Returns null on failure. */
    static String tokenFromConsentResult(Activity activity, Intent data) {
        try {
            AuthorizationResult r = Identity.getAuthorizationClient(activity)
                    .getAuthorizationResultFromIntent(data);
            return r.getAccessToken();
        } catch (Exception e) {
            Log.w(TAG, "consent failed", e);
            return null;
        }
    }
}
