NVIDIA Streamline SDK 2.14.1 public headers (https://github.com/NVIDIA-RTX/Streamline/tree/v2.14.1/include),
MIT license (the notice is kept in each header). Used by vk_frame_gen.cpp, which loads
sl.interposer.dll at run time; no Streamline binary is part of this repository
(tools/fetch_streamline.sh downloads the SDK release).
One local fix: sl_pcl.h's C++23 branch named a function template in an alias declaration (ill-formed); it is a using-declaration here.
