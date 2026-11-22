from pathlib import Path
import subprocess, sys, tempfile
r=Path(__file__).resolve().parents[1]
s=(r/'headless/graphics.cpp').read_text()
body=s.split('        if (sample_start < 0) {',1)[1].split('\n#endif\n        clock.Present(now);',1)[0]
code='''#include <algorithm>
#include <cstdio>
struct Sample {double sample_start{-1},prior_sample_frame{},sample_worst{};unsigned sample_frames{},presented_frames{};
void present(double now){++presented_frames;if(sample_start<0){'''+body+'''} };
int main(){Sample first;for(int i=0;i<=100;++i)first.present(100+i/20.0);
first.present(111); // Include slow intervals rather than silently exclude them.
Sample second;for(int i=0;i<=150;++i)second.present(200+i/30.0);}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++20',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 rows=subprocess.check_output([str(p/'test')],text=True).splitlines()
 assert len(rows)==3,rows
 assert 'frames=100 seconds=5.000000 fps=20.000' in rows[0],rows
 assert 'frames=1 seconds=6.000000 fps=0.167 worst_ms=6000.000' in rows[1],rows
 assert 'frames=150 seconds=5.000000 fps=30.000' in rows[2],rows
 opengl = p / 'opengl.log'
 vulkan = p / 'vulkan.log'
 mixed = p / 'mixed.log'
 opengl.write_text('\n'.join(rows[:2]) + '\n')
 vulkan.write_text(rows[2].replace('EDEN_GAME_FRAME', 'EDEN_VULKAN_FRAME') + '\n')
 mixed.write_text(opengl.read_text() + vulkan.read_text())
 analyzer = [sys.executable, str(r / 'tools/analyze-perf-log.py')]
 result = subprocess.check_output([*analyzer, '--windows', str(opengl), str(vulkan)], text=True)
 assert 'OpenGL, 2 frame windows, 101 frames over 11.0s' in result, result
 assert 'Vulkan, 1 frame windows, 150 frames over 5.0s' in result, result
 assert 'game FPS  mean 9.18' in result and 'worst frame  6000.00 ms' in result, result
 assert 'side by side:' in result, result
 rejected = subprocess.run([*analyzer, str(mixed)], capture_output=True, text=True)
 assert rejected.returncode == 1 and 'mixed Vulkan and OpenGL reports' in rejected.stderr, rejected
print('Normal frame summaries: actual intervals, slow gaps, independent sessions, and OpenGL/Vulkan analysis PASS')
