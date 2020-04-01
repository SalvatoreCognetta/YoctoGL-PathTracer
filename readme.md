# Yocto/Pathtrace: Tiny Path Tracer

In this homework, you will learn how to build a simple path tracer with enough 
features to make it robust for many scenes. In particular, you will learn how to 

- write camera with depth of field,
- write a complex material,
- write a naive path tracer,
- write a path tracer with multiple importance sampling.

## Framework

The code uses the library [Yocto/GL](https://github.com/xelatihy/yocto-gl),
that is included in this project in the directory `yocto`. 
We suggest to consult the documentation for the library that you can find 
at the beginning of the header files. Also, since the library is getting improved
during the duration of the course, se suggest that you star it and watch it 
on Github, so that you can notified as improvements are made. 
In particular, we will use

- **yocto_math.h**: collection of math functions
- **yocto_image.{h,cpp}**: image data structure and image loading and saving 
- **yocto_commonio.h**: helpers for writing command line apps
- **yocto_gui.{h,cpp}**: helpers for writing simple GUIs

In order to compile the code, you have to install 
[Xcode](https://apps.apple.com/it/app/xcode/id497799835?mt=12)
on OsX, [Visual Studio 2019](https://visualstudio.microsoft.com/it/vs/) on Windows,
or a modern version of gcc or clang on Linux, 
together with the tools [cmake](www.cmake.org) and [ninja](https://ninja-build.org).
The script `scripts/build.sh` will perform a simple build on OsX.
As discussed in class, we prefer to use 
[Visual Studio Code](https://code.visualstudio.com), with
[C/C++](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) and
[CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) 
extensions, that we have configured to use for this course.

You will write your code in the file `yocto_pathtrace.cpp` for functions that 
are declared in `yocto_pathtrace.h`. Your renderer is callsed by `yscenetrace.cpp` 
for a command-line interface and `ysceneitraces.cpp` that show a simple 
user interface.

This repository also contains tests that are executed from the command line
as shown in `run.sh`. The rendered images are saved in the `out/` directory. 
The results should match the ones in the directory `check/`.

## Functionality

In this homework you will implement the following features:

- **Camera Sampling** in functions `sample_camera()` and `eval_camera()`:
    - implement camera sampling using `sample_disk()` for the lens
    - implement camera ray generation by simulating a thin lens camera
    - follow the slides to understand how to structure the code
- **Naive Path tracing** in function `trace_naive()`:
    - implement a naive path tracer using the product formulation
    - you should handle both delta and non-delta brdfs using `is_delta()` 
      and the functions below
    - follow the slides to understand how to structure the code
    - you can use the functions `eval_position()`, `eval_shading_normal()`, 
      `eval_emission()`, `eval_brdf()`, `eval_opacity()`
- **Brdf sampling** in function `eval_brdfcos()`, `sample_brdscos()` 
  and `sample_brdfcos_pdf()`:
    - implement brdf evaluation and sampling in the above functions
    - the brdf is a sum of the following lobes stored in a brdf objects
        - diffuse lobe with weight `diffuse`
        - specular lobe with weight `specular`, ior `ior`, 
          and roughness `roughness`
        - metal lobe with weight `metal`, complex ior `meta` and `metak`, 
          and roughness `roughness`
        - transmission lobe with weight `transmission`, ior `ior`, 
          and roughness `roughness`
    - you can use all the reflectance functions in Yocto/Math including `eval_<lobe>()`
      `sample_<lobe>()`, and `sample_<lobe>_pdf()` with lobes
      `<func>_diffuse_reflection()`, `<func>_microfacet_reflection()`,
      `<func>_microfacet_transmission()`
    - `eval_brdfcos()` is just a sum of lobes, but remember to fold in the cosine
    - `sample_brdfcos()` picks a direction based on one of the lobes
    - `sample_brdfcos_pdf()` is the sum of the PDFs using weights `<lobe>_pdf` 
      stored in `brdf`
    - follow the slides to understand how to structure the code
- **Delta handling** in function `eval_delta()`, `sample_delta()` and `sample_delta_pdf()`:
    - same as above with the corresponding functions
    - follow the slides to understand how to structure the code
- **Light sampling** in function `sample_lights()` and `sample_lights_pdf()`:
    - implement light sampling for both area lights and environment maps
    - lights and their CDFs are already implemented in `init_lights()`
    - follow the slides to understand how to structure the code
- **Path tracing** in function `trace_path()`:
    - implement a path tracer in the product formulation that uses MIS for 
      illumination of the smooth BRDFs
    - the simplest way here is to get naive path tracing to work, 
      then cut&paste that code and finally add light sampling using MIS
    - follow the slides to understand how to structure the code

To help out, we left example code in `trace_eyelight()`. You can also check out
Yocto/Trace that implements a similar path tracer; in this case though pay 
attention to the various differences. In our opinion, it is probably easier to 
follow the slides than to follow Yocto/Trace.

## Extra Credit

Here we put options of things you could try to do. 
You do not have to do them all, but maybe try to make your own scene if you can.

- **Refraction** in all BRDFs functions:
    - use the functions in Yocto/Math that directly support refraction
- **MYOS**, make your own scene:
    - create additional scenes that you can render from models assembled by you
    - to create new scenes, you can directly edit the json files that are just
      a serialization of the same-name variables in Yocto/SceneIO
    - remember that to get proper lighting yoou should either use environment 
      maps or emissive materials
        - you can find high quality environment maps on [HDRIHaven](https://hdrihaven.com)
    - as a starting point you could use one of the test scenes and put new objects and environments
        - for material textures, try to search for "free PBR textures" on Google
        - for 3D models I am not sure; you can try either [CGTrader](http://ccgtrader.com) or maybe [SketchFab](http://www.sketchfab.com), or create your own in Blender; you can use either OBJ or PLY models
    - you could also try to get a whole scene in glTF or OBJ, say from Blender or CGTrader and translate it via `ysceneproc` in Yocto/GL
        - note though that in general lights and materials are not properly exported 
- **Render Larger Scenes** in high quality at 1280 resolution with 4096 samples:
    - for fun, we will distribute larger scenes for you to try to render them
    - this is to give you a better sense of the what your renderer can do
    - for speed, you may want to add support for the Embree ray tracing kernels as in Yocto/Trace
    - render this only if you have a fast machine or push them to the cloud;
      if you can manage to get cloud rendering working, let me know how you did it

## Submission

To submit the homework, you need to pack a ZIP file that contains the code 
you write and the images it generates, i.e. the ZIP _with only the 
`yocto_pathtrace/` and `out/` directories_.
The file should be called `<lastname>_<firstname>_<studentid>.zip` 
(`<cognome>_<nome>_<matricola>.zip`) and you should exclude 
all other directories. Send it on Google Classroom.
