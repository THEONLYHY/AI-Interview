#!/usr/bin/env python3
# 生成一段 16 kHz / mono / s16le 的 PCM 测试音频。
#
# 用途：
#   配合 PulseAudio module-pipe-source 做虚拟麦克风，
#   在没有真实麦克风的主机上验证 audio 路径与服务端连接，
#   并让服务端 ASR 真正能识别出文字（不再是合成谐波）。
#
# 实现：
#   使用 edge-tts（Microsoft Edge 在线 TTS）合成中文 mp3，
#   再用 ffmpeg 重采样成与 audio_input 配置一致的裸 PCM。
#
# 依赖：
#   - edge-tts : pip install edge-tts
#   - ffmpeg   : sudo apt install -y ffmpeg
#   - 网络     : edge-tts 通过 WebSocket 访问 Microsoft 在线服务
#
# 输出：
#   scripts/test_voice.pcm
#   （s16le, 16 kHz, mono，与 config/local_config.json 的
#    audio_input.sample_rate / channels / bit_size 对齐）
#
# 用法：
#   python3 scripts/gen_test_voice.py
#   python3 scripts/gen_test_voice.py --text "你好，请做一个自我介绍。"
#   python3 scripts/gen_test_voice.py --text-file my_text.txt
#   python3 scripts/gen_test_voice.py --voice zh-CN-YunxiNeural
#   python3 scripts/gen_test_voice.py --keep-mp3      # 同名 .mp3 留下方便试听
#
# 常用中文 voice：
#   zh-CN-XiaoxiaoNeural   女声，默认
#   zh-CN-YunxiNeural      男声
#   zh-CN-XiaoyiNeural     女声
#   完整列表：edge-tts --list-voices | grep zh-

import argparse
import asyncio
import os
import shutil
import subprocess
import sys
import tempfile

# 与 config/local_config.json 的 audio_input.sample_rate 保持一致。
SAMPLE_RATE = 16000

DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"

# 默认是一段贴近面试场景的中文，便于直接喂给虚拟麦克风做端到端验证。
DEFAULT_TEXT = (
    "你好，我叫张明，今年二十六岁，毕业于武汉理工大学计算机专业。"
    "我有三年后端开发经验，主要使用 C++ 和 Python，"
    "熟悉网络编程、协议设计以及多线程并发，"
    "也参与过基于 WebSocket 的实时通信项目。"
)


def require_module(name: str, install_hint: str) -> None:
    """缺少模块时给出明确安装提示，而不是抛 ImportError 让人摸不着头脑。"""
    try:
        __import__(name)
    except ImportError:
        sys.stderr.write(f"error: missing python module '{name}'\n")
        sys.stderr.write(f"install with: {install_hint}\n")
        sys.exit(1)


def require_cmd(cmd: str, install_hint: str) -> None:
    if shutil.which(cmd) is None:
        sys.stderr.write(f"error: missing command '{cmd}'\n")
        sys.stderr.write(f"install with: {install_hint}\n")
        sys.exit(1)


async def synth_mp3(text: str, voice: str, out_mp3: str) -> None:
    """调用 edge-tts 合成 mp3 到磁盘。"""
    import edge_tts  # 延迟导入,留给 require_module 先报错。

    communicate = edge_tts.Communicate(text, voice=voice)
    await communicate.save(out_mp3)


def mp3_to_pcm(mp3_path: str, pcm_path: str, sample_rate: int) -> None:
    """用 ffmpeg 把 mp3 转成 sample_rate / mono / s16le 的裸 PCM。"""
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-i", mp3_path,
        "-ac", "1",
        "-ar", str(sample_rate),
        "-f", "s16le",
        "-acodec", "pcm_s16le",
        pcm_path,
    ]
    subprocess.run(cmd, check=True)


def parse_args() -> argparse.Namespace:
    default_out = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "test_voice.pcm")

    parser = argparse.ArgumentParser(
        description="Generate 16kHz/mono/s16le PCM test audio via edge-tts.")
    parser.add_argument("-o", "--output", default=default_out,
                        help=f"output pcm path (default: {default_out})")
    parser.add_argument("--text", default=None,
                        help="text to synthesize; overrides --text-file.")
    parser.add_argument("--text-file", default=None,
                        help="read text from a utf-8 file.")
    parser.add_argument("--voice", default=DEFAULT_VOICE,
                        help=f"edge-tts voice name (default: {DEFAULT_VOICE})")
    parser.add_argument("--keep-mp3", action="store_true",
                        help="keep the intermediate mp3 next to the pcm output.")
    # 兼容旧用法: python3 gen_test_voice.py <output_path>
    parser.add_argument("positional_output", nargs="?", default=None,
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.positional_output is not None:
        args.output = args.positional_output
    return args


def resolve_text(args: argparse.Namespace) -> str:
    if args.text is not None:
        text = args.text
    elif args.text_file is not None:
        with open(args.text_file, "r", encoding="utf-8") as f:
            text = f.read()
    else:
        text = DEFAULT_TEXT
    return text.strip()


def main() -> int:
    args = parse_args()

    require_module("edge_tts", "pip install edge-tts")
    require_cmd("ffmpeg", "sudo apt install -y ffmpeg")

    text = resolve_text(args)
    if not text:
        sys.stderr.write("error: empty text\n")
        return 1

    out_pcm = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(out_pcm) or ".", exist_ok=True)

    if args.keep_mp3:
        mp3_path = os.path.splitext(out_pcm)[0] + ".mp3"
    else:
        tmp = tempfile.NamedTemporaryFile(
            prefix="edge_tts_", suffix=".mp3", delete=False)
        tmp.close()
        mp3_path = tmp.name

    try:
        print(f"[tts] voice={args.voice}, chars={len(text)}")
        asyncio.run(synth_mp3(text, args.voice, mp3_path))

        mp3_size = os.path.getsize(mp3_path) if os.path.exists(mp3_path) else 0
        if mp3_size == 0:
            sys.stderr.write(
                "error: edge-tts produced empty mp3.\n"
                "  possible causes:\n"
                "    - no network access to Microsoft Edge TTS service\n"
                "    - invalid voice name (try: edge-tts --list-voices)\n"
                "    - text contains only unsupported characters\n")
            return 2
        print(f"[tts] mp3 = {mp3_path} ({mp3_size} bytes)")

        mp3_to_pcm(mp3_path, out_pcm, SAMPLE_RATE)
    finally:
        if not args.keep_mp3 and os.path.exists(mp3_path):
            try:
                os.remove(mp3_path)
            except OSError:
                pass

    pcm_bytes = os.path.getsize(out_pcm)
    duration = pcm_bytes / (SAMPLE_RATE * 2)
    print(f"[tts] wrote {out_pcm}")
    print(f"[tts] format = s16le {SAMPLE_RATE} Hz mono, "
          f"bytes = {pcm_bytes}, duration = {duration:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
