#!/system/bin/sh
# Late-start: ensure the WebUI config directory exists and is writable.
MODDIR=${0%/*}
mkdir -p /data/adb/il2cpp_tool
# Seed an empty allow-list so the WebUI has a file to edit.
if [ ! -f /data/adb/il2cpp_tool/targets.txt ]; then
    cat > /data/adb/il2cpp_tool/targets.txt <<'EOF'
# One package name per line. Edited by the KernelSU WebUI.
# Example:
# com.unity.example
EOF
fi
# Keep a copy next to the module for managers that cannot write /data/adb/il2cpp_tool.
if [ ! -f "$MODDIR/targets.txt" ]; then
    cp /data/adb/il2cpp_tool/targets.txt "$MODDIR/targets.txt" 2>/dev/null || true
fi
chmod 644 /data/adb/il2cpp_tool/targets.txt 2>/dev/null || true
