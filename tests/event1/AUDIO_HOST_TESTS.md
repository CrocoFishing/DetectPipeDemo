# Audio artifact host checks

Run on Windows with Visual C++ Build Tools:

    .\tests\event1\run_audio_tests.ps1

The test compiles the actual wav_writer.c and audio_ring_buffer.c implementations with small host dependency shims. It independently inspects saved WAV bytes and retained raw sample values. Injected storage failures exercise short writes, header writes, flushes and closes; interrupted recordings exercise header repair. The production queue admission policy is tested with a stalled writer at every queue occupancy to verify that admitted audio events retain an END slot.

The C test also builds with a C11 host compiler by adding the host_stubs directory and the audio common, recorder and ring include directories. FreeRTOS shims run single-threaded; they do not establish ESP32 scheduling, DMA continuity, physical SD reliability or microphone/VAD accuracy. Those require the Event 1 hardware matrix.
