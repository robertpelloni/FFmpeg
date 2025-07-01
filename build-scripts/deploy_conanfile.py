from conan import ConanFile
from conan.tools.files import copy, collect_libs
import os

class conanRecipe(ConanFile):
    name = "topaz-ffmpeg"
    settings = "os", "build_type", "arch"

    def configure(self):
        self.options["zimg"].shared = True
        if self.settings.os == "Macos" or self.settings.os == "Linux":
            self.options["libvpx"].shared = True

    def requirements(self):
        self.requires("videoai/1.9.29-win2022+1")
        if self.settings.os == "Macos" and self.settings.arch == "x86_64":
            self.requires("zimg/3.0.5@josh/oiio3")
        else:
            self.requires("zimg/3.0.5")
        if self.settings.os == "Macos" or self.settings.os == "Linux":
            self.requires("libvpx/1.14.1") #libvpx is static on Windows
            self.requires("libaom-av1/3.5.0")
            
    def package_id(self):
        self.info.requires["videoai"].minor_mode()

    def package(self):
        # Copy header files from include folder or root
        src_include = os.path.join(self.source_folder, "include")
        if os.path.exists(src_include):
            copy(
                self,
                "*",
                src=src_include,
                dst=os.path.join(self.package_folder, "include"),
                keep_path=True,
            )
        else:
            # Copy header files from root if no include folder
            copy(
                self,
                "*.h",
                src=self.source_folder,
                dst=os.path.join(self.package_folder, "include"),
                keep_path=True,
            )
        
        # Copy libraries to lib folder (from any location in source)
        copy(
            self,
            "*.lib",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "lib"),
            keep_path=False,
        )
        copy(
            self,
            "*.a",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "lib"),
            keep_path=False,
        )
        copy(
            self,
            "*.so*",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "lib"),
            keep_path=False,
        )
        copy(
            self,
            "*.dylib",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "lib"),
            keep_path=False,
        )
        
        # Copy DLLs and executables to bin folder (from any location in source)
        copy(
            self,
            "*.dll",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "bin"),
            keep_path=False,
        )
        copy(
            self,
            "*.exe",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "bin"),
            keep_path=False,
        )
        
        # Copy pkg-config files
        copy(
            self,
            "*.pc",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "lib", "pkgconfig"),
            keep_path=False,
        )

    def package_info(self):
        # Define individual components for each FFmpeg library
        
        # avutil - core utility library (base for others)
        self.cpp_info.components["avutil"].libs = ["avutil"]
        
        # avcodec - codec library
        self.cpp_info.components["avcodec"].libs = ["avcodec"]
        self.cpp_info.components["avcodec"].requires = ["avutil"]
        
        # avformat - format library
        self.cpp_info.components["avformat"].libs = ["avformat"]
        self.cpp_info.components["avformat"].requires = ["avutil", "avcodec"]
        
        # avfilter - filter library
        self.cpp_info.components["avfilter"].libs = ["avfilter"]
        self.cpp_info.components["avfilter"].requires = ["avutil"]
        
        # avdevice - device library
        self.cpp_info.components["avdevice"].libs = ["avdevice"]
        self.cpp_info.components["avdevice"].requires = ["avutil", "avformat"]
        
        # swscale - scaling library
        self.cpp_info.components["swscale"].libs = ["swscale"]
        self.cpp_info.components["swscale"].requires = ["avutil"]
        
        # swresample - resampling library
        self.cpp_info.components["swresample"].libs = ["swresample"]
        self.cpp_info.components["swresample"].requires = ["avutil"]

    def layout(self):
        folder_dir = self.conf.get("user.path:folder_dir", default=None)
        if not folder_dir:
            folder_dir = f"{self.settings.os}_{self.settings.arch}".lower()

        self.output.info(f"folder_dir: {folder_dir}")
        self.folders.source = folder_dir
        
        self.cpp.package.libs = collect_libs(self)
