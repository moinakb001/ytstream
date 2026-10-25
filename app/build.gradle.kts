import java.util.Properties

plugins {
    id("com.android.application")
}

// Location of the extracted GStreamer Android SDK (gstreamer-1.0-android-universal-<ver>.tar.xz):
// `gstreamer.dir=/path` in local.properties, or the GSTREAMER_ROOT_ANDROID environment variable.
val gstreamerRoot: String = run {
    val props = Properties()
    rootProject.file("local.properties").takeIf { it.exists() }?.inputStream()?.use { props.load(it) }
    props.getProperty("gstreamer.dir") ?: System.getenv("GSTREAMER_ROOT_ANDROID")
        ?: throw GradleException("Set gstreamer.dir in local.properties or GSTREAMER_ROOT_ANDROID (see README)")
}

// GStreamer's Java glue (org.freedesktop.gstreamer.*) and CA bundle come from the SDK.
val gstGenerated = layout.buildDirectory.dir("generated/gstreamer")
val stageGStreamer by tasks.registering {
    val sdk = file("$gstreamerRoot/arm64")
    inputs.dir("$sdk/share/gst-android/ndk-build")
    inputs.file("$sdk/etc/ssl/certs/ca-certificates.crt")
    outputs.dir(gstGenerated)
    doLast {
        val out = gstGenerated.get().asFile
        val javaDir = File(out, "java/org/freedesktop/gstreamer")
        javaDir.mkdirs()
        val ndkBuild = File(sdk, "share/gst-android/ndk-build")
        // Enable copying the CA bundle so libsoup/OpenSSL can verify HTTPS.
        File(javaDir, "GStreamer.java").writeText(
            File(ndkBuild, "GStreamer.java").readText()
                .replace("//copyCaCertificates(context);", "copyCaCertificates(context);")
        )
        // Java helpers used by the androidmedia (MediaCodec) plugin.
        File(ndkBuild, "androidmedia").copyRecursively(File(javaDir, "androidmedia"), overwrite = true)
        val certs = File(out, "assets/ssl/certs")
        certs.mkdirs()
        File(sdk, "etc/ssl/certs/ca-certificates.crt").copyTo(File(certs, "ca-certificates.crt"), overwrite = true)
    }
}

android {
    namespace = "com.ytstream"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.ytstream"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DGSTREAMER_ROOT_ANDROID=$gstreamerRoot",
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets["main"].java.srcDir(gstGenerated.map { it.dir("java") })
    sourceSets["main"].assets.srcDir(gstGenerated.map { it.dir("assets") })

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

tasks.named("preBuild") { dependsOn(stageGStreamer) }

dependencies {
    // Only used to obtain a YouTube OAuth access token; everything else is native.
    implementation("com.google.android.gms:play-services-auth:21.4.0")
}
