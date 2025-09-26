from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
import os

class NatsppTestConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = []
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        # Just run the example; it should not require a running NATS server
        bin_path = os.path.join(self.cpp.build.bindirs[0], "example")
        self.run(bin_path, env="conanrun")
