#!/bin/sh
# Launcher script for OpenXMB
DIR=$(cd "$(dirname "$0")" && pwd)
# Copy the config file if it doesn't exist, so the user has a default.
if [ ! -f "config.json" ]; then
    cp "../share/OpenXMB/config.json" "config.json"
fi

# On macOS, help the Vulkan loader find MoltenVK if not configured.
UNAME=$(uname -s 2>/dev/null)
if [ "$UNAME" = "Darwin" ]; then
    if [ -z "$VK_ICD_FILENAMES" ]; then
        # Common Homebrew prefixes
        for PREFIX in "/opt/homebrew" "/usr/local"; do
            CAND="$PREFIX/share/vulkan/icd.d/MoltenVK_icd.json"
            if [ -f "$CAND" ]; then
                export VK_ICD_FILENAMES="$CAND"
                break
            fi
        done
        # Also check etc paths used by Homebrew
        if [ -z "$VK_ICD_FILENAMES" ]; then
            for PREFIX in "/opt/homebrew" "/usr/local"; do
                CAND="$PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"
                if [ -f "$CAND" ]; then
                    export VK_ICD_FILENAMES="$CAND"
                    break
                fi
            done
        fi
        # Fallback to Vulkan SDK if installed
        if [ -z "$VK_ICD_FILENAMES" ] && [ -n "$VULKAN_SDK" ]; then
            CAND="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
            if [ -f "$CAND" ]; then
                export VK_ICD_FILENAMES="$CAND"
            fi
        fi
        # Fallback to Homebrew Cellar (in case formula is not linked)
        if [ -z "$VK_ICD_FILENAMES" ]; then
            for C in \
                "/usr/local/Cellar/molten-vk"/*"/share/vulkan/icd.d/MoltenVK_icd.json" \
                "/opt/homebrew/Cellar/molten-vk"/*"/share/vulkan/icd.d/MoltenVK_icd.json"; do
                [ -f "$C" ] && { export VK_ICD_FILENAMES="$C"; break; }
            done
        fi
        # Last hint: Homebrew opt path (if linked)
        if [ -z "$VK_ICD_FILENAMES" ] && command -v brew >/dev/null 2>&1; then
            OPT_PREFIX=$(brew --prefix molten-vk 2>/dev/null || true)
            if [ -n "$OPT_PREFIX" ]; then
                CAND="$OPT_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json"
                [ -f "$CAND" ] && export VK_ICD_FILENAMES="$CAND"
            fi
        fi
    fi
fi

# Default assets dir (if not set) -> installed share
if [ -z "$XMB_ASSET_DIR" ]; then
    if [ -d "$DIR/../share/shell" ]; then
        export XMB_ASSET_DIR="$DIR/../share/shell"
    fi
fi
exec "$DIR/XMS.bin" "$@"
