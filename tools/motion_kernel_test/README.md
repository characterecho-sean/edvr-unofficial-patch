# Motion-kernel GPU regression

From the repository root, with Python/numpy and Visual Studio 2022 available:

```
python tools/motion_kernel_test/prepare.py
tools\motion_kernel_test\run.bat
```

The D3D11 hardware runner compares all four reconstruction inputs from the
production tiled shader (normal and diagnostic builds) with a scalar depth
neighborhood reference. Fixtures cover cropped source textures, partial and
one-pixel workgroups, separate smoke/UI depth, body/ship motion and history
rejection. Results are in `build/motion_kernel_test/results.txt`.

This checks equivalence of the optimization. It does not validate the game's
motion inputs or measure end-to-end frame rate.
