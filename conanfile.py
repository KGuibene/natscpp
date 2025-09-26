from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
import os

class NatsppConan(ConanFile):
    name = "natspp"
    version = "0.1.0"
    package_type = "library"

    # Binary compatibility settings
    settings = "os", "arch", "compiler", "build_type"

    # Options
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_demo": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_demo": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "include/*",
        "src/*",
        "tests/*", 
        "proto/*",
        "cmake/*",
        "LICENSE*",
        "README*",
    )

    def layout(self):
        cmake_layout(self)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["NATSPP_BUILD_DEMO"] = "ON" if self.options.with_demo else "OFF"
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["natspp"]
        # Provide CMake target alias
        self.cpp_info.set_property("cmake_file_name", "natspp")
        self.cpp_info.set_property("cmake_target_name", "natspp::natspp")
