# Synthetic plain Baseline regression fixture

`plain-baseline-1080p.h264` contains one synthetic red 1920x1080 frame, not camera footage. Generated locally with libx264 baseline, level 4.0, no B frames, and no FMO/ASO. Only SPS constraint_set1_flag was cleared, so the identical coding tools are identified as plain Baseline (profile_idc=66) instead of Constrained Baseline. This reproduces the Samsung sender/FFmpeg DXVA profile-selection mismatch.

The hardware decoder test verifies an actual NV12 D3D11 frame. A small portable test alone cannot establish GPU interoperability.
