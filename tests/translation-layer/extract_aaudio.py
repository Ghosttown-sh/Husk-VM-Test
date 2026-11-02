#!/usr/bin/env python3
"""Compile the actual AAudio adapter, excluding unrelated NDK dependencies."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
path = Path(sys.argv[2]) if len(sys.argv) > 2 else root / 'src/translation-layer-next/husk-tl-bionic-ndk.c'
source = path.read_text()
start = source.index('extern void (*tl_cocos_audio_hook)')
end = source.index('/* AAssetDir:')
assert 'TL_WRAP("AAudioStreamBuilder_setSampleRate", b_AAudioStreamBuilder_setSampleRate)' in source, 'Sample-rate setter must be bound in the actual symbol table'
Path(sys.argv[1]).write_text(source[start:end])
