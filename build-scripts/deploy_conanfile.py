from conans import ConanFile, tools

class conanRecipe(ConanFile):
	name = "topaz-ffmpeg"
	settings = ("os", "build_type", "arch")

	def requirements(self):
		self.requires("videoai/0.6.19")
		if self.settings.os == "Macos":
		    self.requires("openh264/2.2.0")
		    self.requires("libvpx/1.11.0")

	def imports(self):
		if self.settings.os == "Windows":
			self.copy("*")
		if self.settings.os == "Macos":
			self.copy("*")
