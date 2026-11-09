# Water Physics

Vehicles float: lift at four points of the footprint in proportion to depth, and drag against speed, so a vehicle tips and settles instead of bobbing; it takes on water and sinks over SinkSeconds. Set the sea level once with Ctrl+F8 at the water's edge (nothing floats until then).



Settings: `Binaries\Win64\plugins\water_physics.ini`.

## Install

Needs [Plugin Loader](https://github.com/qxZap/mt-plugin-loader) (`version.dll`) in
`MotorTown\Binaries\Win64\`. Put `mt_plugin_WaterPhysics.dll` in `MotorTown\Binaries\Win64\plugins\`.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_WaterPhysics.dll`.
