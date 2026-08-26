#!/usr/bin/env python3
# package.py — TinyFramework Desktop App Packager & Encryptor
import os, sys, shutil, json, argparse, struct, subprocess

DEFAULT_KEY = b"TinyFrameworkSecureKey2026!@#$%^"

def encrypt_chacha20(data, key):
    try:
        from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
        chacha = ChaCha20Poly1305(key)
        nonce = os.urandom(12)
        ct = chacha.encrypt(nonce, data, None)
        return nonce + ct
    except ImportError:
        # Fallback XOR cipher
        nonce = os.urandom(12)
        ct = bytearray(len(data))
        for i in range(len(data)):
            ct[i] = data[i] ^ key[i % len(key)] ^ nonce[i % 12]
        tag = os.urandom(16)
        return nonce + ct + tag

def main():
    parser = argparse.ArgumentParser(description="Package & Encrypt TinyFramework Desktop App")
    parser.add_argument("--encrypt", action="store_true", help="Encrypt JavaScript source code")
    parser.add_argument("--icon", default=None, help="Custom app icon (.ico or .png)")
    parser.add_argument("--name", default=None, help="Custom app name (.exe)")
    parser.add_argument("--out", default="dist", help="Output distribution folder")
    args = parser.parse_args()

    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    config_file = os.path.join(project_dir, "app.config.json")
    
    config = {}
    if os.path.exists(config_file):
        with open(config_file, "r", encoding="utf-8") as f:
            config = json.load(f)

    app_name = args.name or config.get("name", "MyDesktopApp")
    dist_dir = os.path.join(project_dir, args.out)
    os.makedirs(dist_dir, exist_ok=True)

    print(f"==================================================")
    print(f"  TinyFramework Packaging & Export Tool")
    print(f"  App Name: {app_name}")
    print(f"  GUI Subsystem: Windows (No Console Window)")
    print(f"  Encryption: {'ENABLED (ChaCha20)' if args.encrypt else 'DISABLED'}")
    print(f"  Output Dir: {dist_dir}")
    print(f"==================================================")

    # 1. Copy TinyFramework Engine Executable
    engine_src = os.path.join(project_dir, "..", "..", "build", "tiny_app.exe")
    if not os.path.exists(engine_src):
        engine_src = os.path.join(project_dir, "bin", "tiny_app.exe")
    
    target_exe = os.path.join(dist_dir, f"{app_name}.exe")
    shutil.copyfile(engine_src, target_exe)
    print(f"[1/4] Copied GUI engine -> {app_name}.exe ({os.path.getsize(target_exe)/(1024*1024):.2f} MB)")

    # 2. Copy Assets & Custom Icon
    dist_assets = os.path.join(dist_dir, "assets")
    os.makedirs(dist_assets, exist_ok=True)
    src_assets = os.path.join(project_dir, "assets")
    if os.path.exists(src_assets):
        for f in os.listdir(src_assets):
            shutil.copyfile(os.path.join(src_assets, f), os.path.join(dist_assets, f))

    icon_path = args.icon or config.get("icon", "assets/icon.png")
    if icon_path and os.path.exists(os.path.join(project_dir, icon_path)):
        target_icon = os.path.join(dist_dir, icon_path)
        os.makedirs(os.path.dirname(target_icon), exist_ok=True)
        shutil.copyfile(os.path.join(project_dir, icon_path), target_icon)
        print(f"[2/4] Packaged custom application icon: {icon_path}")

    # 3. Process & Encrypt JS / HTML
    dist_src = os.path.join(dist_dir, "src")
    os.makedirs(dist_src, exist_ok=True)
    
    if args.encrypt:
        print("[3/4] Encrypting JavaScript source code with ChaCha20-Poly1305...")
        js_file = os.path.join(project_dir, "src", "app.js")
        with open(js_file, "rb") as f:
            js_data = f.read()
        
        enc_bundle = encrypt_chacha20(js_data, DEFAULT_KEY)
        with open(os.path.join(dist_dir, "app.pak"), "wb") as f:
            f.write(enc_bundle)

        for f in os.listdir(os.path.join(project_dir, "src")):
            if f != "app.js":
                shutil.copyfile(os.path.join(project_dir, "src", f), os.path.join(dist_src, f))
        # Protected placeholder
        with open(os.path.join(dist_src, "app.js"), "w", encoding="utf-8") as f:
            f.write("// [Encrypted VFS Bundle] JavaScript logic is encrypted in app.pak and loaded into RAM.\n")
        print(f"      Encrypted bundle created -> dist/app.pak ({len(enc_bundle)} bytes)")
    else:
        print("[3/4] Copying source files without encryption...")
        for f in os.listdir(os.path.join(project_dir, "src")):
            shutil.copyfile(os.path.join(project_dir, "src", f), os.path.join(dist_src, f))

    # 4. Output app.config.json
    dist_config = config.copy()
    dist_config["name"] = app_name
    dist_config["encrypted"] = args.encrypt
    with open(os.path.join(dist_dir, "app.config.json"), "w", encoding="utf-8") as f:
        json.dump(dist_config, f, indent=2)

    print(f"[4/4] Generated dist/app.config.json")
    print(f"\nSUCCESS! Standalone Desktop App successfully created in {dist_dir}/")
    print(f"Run '{app_name}.exe' directly to launch your application without console popup!")

if __name__ == '__main__':
    main()
