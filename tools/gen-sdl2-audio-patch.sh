#!/bin/bash
# Generate workspace/miyoomini/other/sdl2-audio-persist.patch against the real pinned SDL2 tree.
# RUNS INSIDE THE TOOLCHAIN CONTAINER. Emitting via `git diff` means the patch is correct by
# construction rather than hand-authored and hoped for (same discipline as the core patches).
set -e

SRC=https://github.com/XK9274/sdl2_miyoo.git
BR=pico8
OUT=/root/workspace/miyoomini/other/sdl2-audio-persist.patch
W=/tmp/sdl2gen

rm -rf "$W"
git clone -q --depth 1 --branch "$BR" "$SRC" "$W"
cd "$W"

python3 - <<'PY'
f = 'src/audio/mmiyoo/SDL_audio_mmiyoo.c'
s = open(f).read()

# ---- 1. CloseDevice: stop powering the codec down. That power-down IS the exit pop. ----
old_close = """    SDL_free(this->hidden->mixbuf);
    SDL_free(this->hidden);
#if defined(MMIYOO)
    MI_AO_DisableChn(AoDevId, AoChn);
    MI_AO_Disable(AoDevId);
#endif"""
new_close = """    SDL_free(this->hidden->mixbuf);
    SDL_free(this->hidden);
#if defined(MMIYOO)
    /* Deliberately DO NOT disable the device here.
       MI_AO_Disable powers the analog stage down, and that transition IS the audible pop on game
       exit -- measured on device: holding the codec open made the pop vanish entirely, while
       muting and rail-settling around the disable changed nothing.
       The codec is left enabled for the next process to adopt; MMIYOO_OpenDevice below handles
       the already-enabled case explicitly. */
#endif"""
assert old_close in s, 'CloseDevice hunk not found'
s = s.replace(old_close, new_close, 1)

# ---- 2. OpenDevice: adopt an already-enabled device, but ONLY if its config matches. ----
old_open = """    miret = MI_AO_SetPubAttr(AoDevId, &stSetAttr);
    if(miret != MI_SUCCESS) {
        printf("%s, failed to set PubAttr\\n", __func__);
        return -1;
    }
    miret = MI_AO_GetPubAttr(AoDevId, &stGetAttr);
    if(miret != MI_SUCCESS) {
        printf("%s, failed to get PubAttr\\n", __func__);
        return -1;
    }
    miret = MI_AO_Enable(AoDevId);
    if(miret != MI_SUCCESS) {
        printf("%s, failed to enable AO\\n", __func__);
        return -1;
    }
    miret = MI_AO_EnableChn(AoDevId, AoChn);
    if(miret != MI_SUCCESS) {
        printf("%s, failed to enable Channel\\n", __func__);
        return -1;
    }"""

new_open = """    /* The device may still be ENABLED by a previous process, because CloseDevice above no longer
       disables it. MI_AO_SetPubAttr cannot reconfigure a live device and returns 0xa0052009.
       The stock driver treated that as fatal, which is why keeping the codec open used to leave
       every game after the first without audio. */
    adopted = 0;
    miret = MI_AO_SetPubAttr(AoDevId, &stSetAttr);
    if(miret != MI_SUCCESS) {
        /* Read back what the device is ACTUALLY configured for. Adopt it only if that matches
           what we need -- silently playing through a mismatched configuration is exactly the
           failure mode we are trying to remove, and it is indistinguishable from "no audio". */
        if(MI_AO_GetPubAttr(AoDevId, &stGetAttr) == MI_SUCCESS &&
           stGetAttr.eSamplerate == stSetAttr.eSamplerate &&
           stGetAttr.u32ChnCnt   == stSetAttr.u32ChnCnt &&
           stGetAttr.eSoundmode  == stSetAttr.eSoundmode) {
            printf("%s, adopting live device (%d Hz, %d ch) -- no reconfigure, no pop\\n",
                   __func__, (int)stGetAttr.eSamplerate, (int)stGetAttr.u32ChnCnt);
            adopted = 1;
        }
        else {
            /* Genuinely different format. We MUST reconfigure, and that requires a power-down.
               Accept one pop here rather than play at the wrong rate. In practice every core on
               this device resolves to the same 48 kHz output, so this path is rare. */
            printf("%s, live device has a different format -- reconfiguring (expect one pop)\\n", __func__);
            MI_AO_DisableChn(AoDevId, AoChn);
            MI_AO_Disable(AoDevId);
            miret = MI_AO_SetPubAttr(AoDevId, &stSetAttr);
            if(miret != MI_SUCCESS) {
                printf("%s, failed to set PubAttr\\n", __func__);
                return -1;
            }
        }
    }
    if(!adopted) {
        miret = MI_AO_GetPubAttr(AoDevId, &stGetAttr);
        if(miret != MI_SUCCESS) {
            printf("%s, failed to get PubAttr\\n", __func__);
            return -1;
        }
        miret = MI_AO_Enable(AoDevId);
        if(miret != MI_SUCCESS) {
            printf("%s, failed to enable AO\\n", __func__);
            return -1;
        }
    }
    /* Re-enabling a channel that is already enabled is expected when adopting, not an error. */
    miret = MI_AO_EnableChn(AoDevId, AoChn);
    if(miret != MI_SUCCESS && !adopted) {
        printf("%s, failed to enable Channel\\n", __func__);
        return -1;
    }"""
assert old_open in s, 'OpenDevice hunk not found'
s = s.replace(old_open, new_open, 1)

# declare the new local alongside the other MMIYOO locals
old_decl = """    MI_S32 miret = 0;
    MI_S32 s32SetVolumeDb = 0;"""
new_decl = """    MI_S32 miret = 0;
    int adopted = 0;
    MI_S32 s32SetVolumeDb = 0;"""
assert old_decl in s, 'local decl site not found'
s = s.replace(old_decl, new_decl, 1)

open(f, 'w').write(s)
print('edited SDL_audio_mmiyoo.c')
PY

# Scope the diff to the audio driver ONLY. An unscoped `git diff` also picked up a CRLF
# line-ending artifact on the tracked libGLESv2.so blob, which would have shipped a corrupted
# binary inside an "audio" patch.
git diff -- src/audio/mmiyoo/ > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
git checkout -- .
