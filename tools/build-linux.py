#!/usr/bin/env python3
"""Configure and build the native Linux chart editor with Ninja."""

import argparse
from pathlib import Path
import shlex
import subprocess
import os
import shutil
import sys

ROOT = Path(__file__).resolve().parent.parent


def ninja_path(path):
    return str(path).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def source_files():
    excluded_lib = {
        'Core/Win32LeanWindowsHeader.cpp', 'IO/Crypto/Crypto.cpp',
        'IO/Crypto/Detail/Win32Crypto.cpp', 'IO/Directory.cpp', 'IO/File.cpp',
        'IO/Path.cpp', 'IO/Shell.cpp', 'IO/Stream/FileStream.cpp',
        'Misc/UTF8.cpp', 'Time/TimeSpan.cpp', 'Graphics/Utilities/TextureCompression.cpp',
    }
    excluded_engine = {
        'Window/ApplicationHost.cpp', 'ImGui/GuiRenderer.cpp', 'ImGui/ComfyTextureID.cpp',
        'Input/Core/InputSystem.cpp', 'System/Library/LibraryLoader.cpp',
        'Audio/Decoder/DecoderFactory.cpp', 'Render/Core/Renderer2D/Renderer2D.cpp', 'Render/Core/RenderSnapshot.cpp',
    }
    library = []
    for source in sorted((ROOT / 'ComfyLib/src').rglob('*.cpp')):
        if source.relative_to(ROOT / 'ComfyLib/src').as_posix() not in excluded_lib:
            library.append(source)
    engine = []
    for source in sorted((ROOT / 'ComfyEngine/src').rglob('*.cpp')):
        relative = source.relative_to(ROOT / 'ComfyEngine/src').as_posix()
        if relative in excluded_engine:
            continue
        if relative.startswith(('Render/D3D11/', 'Render/Shader/', 'Render/Core/Renderer3D/',
                                'ImGui/Implementation/', 'Audio/Core/Backend/', 'Render/Movie/')):
            continue
        engine.append(source)
    studio = []
    for source in sorted((ROOT / 'ComfyStudio/src').rglob('*.cpp')):
        relative = source.relative_to(ROOT / 'ComfyStudio/src').as_posix()
        if relative in {'EntryPoint.cpp', 'MainTest.cpp'}:
            continue
        if relative.startswith(('Editor/Aet/', 'Editor/PV/', 'DataTest/')):
            continue
        studio.append(source)
    data = ROOT / 'ComfyData/src/Platform/Linux/ComfyDataBuild.cpp'
    return library, engine, studio, data


def configure(build_directory, debug):
    build_directory.mkdir(parents=True, exist_ok=True)
    generated = build_directory / 'generated/Version'
    generated.mkdir(parents=True, exist_ok=True)
    version = (ROOT / 'ComfyStudio/src/Version/BuildVersionDummy.h').read_text()
    version = version.replace('struct Comfy::Studio::BuildVersion', 'namespace Comfy::Studio { struct BuildVersion') + '\n}\n'
    version_path = generated / 'BuildVersion.h'
    if not version_path.exists() or version_path.read_text() != version:
        version_path.write_text(version)
    include_directories = [
        build_directory / 'generated', ROOT / 'ComfyLib/src', ROOT / 'ComfyEngine/src',
        ROOT / 'ComfyStudio/src', ROOT / 'Dependencies/glm/include',
        ROOT / 'Dependencies/stb/include', ROOT / 'Dependencies/rapidjson/include',
        ROOT / 'Dependencies/iconfont/include', ROOT / 'Dependencies/dr_libs/include',
        ROOT / 'Dependencies/discord_game_sdk/include',
    ]
    packages = ['sdl2', 'glew', 'gl', 'openssl', 'zlib', 'vorbisfile', 'vorbis', 'vorbisenc', 'ogg', 'libavformat', 'libavcodec', 'libavutil', 'libswscale']
    cflags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', *packages], text=True))
    libraries = shlex.split(subprocess.check_output(['pkg-config', '--libs', *packages], text=True))
    flags = ['-std=c++17', '-pthread', '-fms-extensions', '-Wno-multichar', '-Wno-abstract-final-class',
             '-DGLM_FORCE_SWIZZLE', '-DGLM_FORCE_DEPTH_ZERO_TO_ONE',
             '-DIMGUI_DISABLE_WIN32_FUNCTIONS', '-DDR_FLAC_NO_STDIO', '-DDR_MP3_NO_STDIO', '-DDR_WAV_NO_STDIO']
    flags += ['-O0', '-g', '-DCOMFY_DEBUG'] if debug else ['-O2', '-DCOMFY_RELEASE', '-DNDEBUG']
    flags += ['-I' + str(directory) for directory in include_directories]
    flags += cflags
    compiler = os.environ.get('CXX', 'clang++')
    lines = [
        'ninja_required_version = 1.7',
        'cxx = ' + shlex.quote(compiler),
        'flags = ' + shlex.join(flags).replace('$', '$$'),
        'libraries = ' + shlex.join(libraries + ['-ldl', '-pthread']).replace('$', '$$'),
        'rule compile',
        '  command = $cxx $flags -MMD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CXX $in',
        'rule archive',
        '  command = ar rcs $out $in',
        'rule link',
        '  command = $cxx $in $libraries -o $out',
        '  description = LINK $out',
        'rule pack_data',
        '  command = ./ComfyDataBuild -build_comfy_data ' + shlex.quote(str(ROOT / 'ComfyData/data-src')).replace('$', '$$') + ' $out',
        '  description = PACK $out',
    ]
    library, engine, studio, data = source_files()
    test_source = ROOT / 'tests/LinuxTests.cpp'
    objects = {}
    for source in library + engine + studio + [data, test_source]:
        output = Path('obj') / source.relative_to(ROOT).with_suffix('.o')
        (build_directory / output.parent).mkdir(parents=True, exist_ok=True)
        objects[source] = ninja_path(output)
        lines.append('build ' + objects[source] + ': compile ' + ninja_path(source))
    lines.append('build libComfyLib.a: archive ' + ' '.join(objects[source] for source in library))
    lines.append('build ComfyDataBuild: link ' + objects[data] + ' libComfyLib.a')
    assets = sorted(path for path in (ROOT / 'ComfyData/data-src').rglob('*') if path.is_file())
    lines.append('build ComfyData.dat: pack_data ComfyDataBuild ' + ' '.join(ninja_path(path) for path in assets))
    lines.append('build ComfyStudio: link ' + ' '.join(objects[source] for source in engine + studio) + ' libComfyLib.a || ComfyData.dat')
    test_objects = [objects[source] for source in engine + studio if source.name != 'EntryPoint.cpp']
    lines.append('build LinuxTests: link ' + objects[test_source] + ' ' + ' '.join(test_objects) + ' libComfyLib.a')
    lines.append('default ComfyStudio ComfyDataBuild')
    (build_directory / 'build.ninja').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/linux')
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--test', action='store_true', help='Run format, rendering, and editor startup tests')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--target', choices=['ComfyStudio', 'ComfyDataBuild', 'libComfyLib.a'])
    arguments = parser.parse_args()
    if not sys.platform.startswith('linux'):
        parser.error('This builder requires Linux; use Comfy.sln on Windows.')
    if arguments.jobs < 1:
        parser.error('--jobs must be positive')
    for executable in [os.environ.get('CXX', 'clang++'), 'ninja', 'pkg-config', 'ar']:
        if shutil.which(executable) is None:
            parser.error(f'Required build tool not found: {executable}')
    build_directory = arguments.build_dir.resolve()
    configure(build_directory, arguments.debug)
    command = ['ninja', '-C', str(build_directory), '-j', str(arguments.jobs)]
    if arguments.test:
        command += ['ComfyStudio', 'ComfyDataBuild', 'LinuxTests']
    elif arguments.target:
        command.append(arguments.target)
    subprocess.run(command, check=True)
    if arguments.test or not arguments.target or arguments.target == 'ComfyStudio':
        shutil.copytree(ROOT / 'ComfyStudio/manual', build_directory / 'manual', dirs_exist_ok=True)
    if arguments.test:
        environment = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy')
        tests = [[], ['--render'], ['--audio'], ['--editor']]
        if shutil.which('ffmpeg'):
            fixture = build_directory / 'linux-video-seek-test.mp4'
            subprocess.run([
                'ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=1920x1080:r=30:d=3',
                '-f', 'lavfi', '-i', 'color=c=blue:s=1920x1080:r=30:d=3',
                '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0[v]', '-map', '[v]',
                '-c:v', 'mpeg4', '-q:v', '3', '-g', '180', '-bf', '2', '-y', str(fixture),
            ], check=True, timeout=30)
            tests.append(['--video', str(fixture)])
        if (build_directory / 'dev_rom').is_dir():
            tests.insert(2, ['--assets'])
        for test_arguments in tests:
            subprocess.run([str(build_directory / 'LinuxTests'), *test_arguments], cwd=build_directory,
                           env=environment, check=True, timeout=30)


if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as error:
        print(f'Linux build failed: {error}', file=sys.stderr)
        sys.exit(1)
