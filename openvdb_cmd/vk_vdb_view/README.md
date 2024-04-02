# Vulkan vdb_view
This is a fork of `vdb_view` which adds Vulkan as the default graphics API, rather than OpenGL. Vulkan VDB View should not be thought of as an upgrade to the OpenGL original. VDB View is too simple of an application to benefit significantly from Vulkan's strengths over OpenGL. Performance and functionality between the OpenGL and Vulkan backends are essentially the same, and building with Vulkan introduces many build and runtime dependencies and compatibility hazards not presented by OpenGL. Vulkan VDB View was implemented primarily as a learning exercise, and jumping off point for possible future Vulkan development. This project is most suited for being a point of reference for similarly scoped Vulkan applications and as a development curiosity, rather than as an addition to the OpenVDB toolset.

Beyond integrating Vulkan, this fork does make some other minor changes to the original
* MSAA support has been added to both the Vulkan and OpenGL. Enabled via the `--msaa|-s` CLI flag.
* Both the Vulkan and OpenGL modes now correctly display to the sRGB colorspace
* The color palette used has been made darker, with a black background and other elements brightened to maintain contrast
* View tumbling has been tweaked so that it remains consistent regardless of zoom level
* Vulkan VDB View attempts to detect high-DPI displays and increases UI font size to maintain readability
* Rendering now continues even when not interacting with the window, and FPS is more accurately measured.
* Misc other small tweaks to presentation and interaction

The Vulkan mode has parity with OpenGL other than one exception, which is the setting of clipping planes. Clipping planes are ignored in the Vulkan version. This is not a technical limitation, it is just yet to be added. 

## Architecture
This repository was initialized by first copying the contents of `openvdb_cmd/vdb_view` from the OpenVDB 9.1 repository. The original source files found in the vdb_view are relocated to `src/` and have also been modified. The bulk of changes to the original source have been made in `Viewer.cc`, where the original viewer implementation has been renamed to `OpenGLViewerImpl` and a sibling `VulkanViewerImpl` has been created. This new viewer implementation is the central class of the Vulkan version of VDB view, and is where the new Vulkan code begins. `RenderModules.cc` is the second most modified file, as Vulkan rendering setup and command recording has been added to implement VDB view's render modules in the new API. For the most part, the existing code for loading VDB data into GPU renderable formats is unchanged, and is just be fed to Vulkan buffers rather than OpenGL buffers. More minor changes have been made to most of the other source files from the original VDB View. 

An effort was made to keep most of the new Vulkan code isolated in new source files, and independent of existing OpenGL sources. This code can be found in `src/vulkan/`, and is all new contributions. Much of this code is more broad, and implements utilities useful to Vulkan development in general.
* `src/vulkan/Utils.*`: The most general header/source pair. Defines a variety of Vulkan utilities used to simplify the API's usage across all other source files. 
    * Utilities are declared within the namespace `vult`
    * `VulkanRuntimeScope` and related classes define utilities for encapsulating and sharing nearly static Vulkan objects and state across the application
    * `DevicePair` & `DeviceBundle` provide wrappers around Vulkan device handles to associate them with related information.
    * `QueueClosure` wraps Vulkan queues, associating them with their contextual information, and providing additional functionality. Specifically, support for one-time-submit operations, ideal for loading data onto the GPU and other one off tasks. 
* `src/vulkan/Resources.*`: Further extension to the `vult` namespace, defining wrappers around Vulkan's buffers and reflecting common usage patterns.
* `src/vulkan/GlfwVulkan.*`: Standalone utility providing a streamlined process for creating and managing GLFW windows targeted for rendering by Vulkan. To increase portability, `GlfwVulkan` does not rely on any of the other headers in this project.
* `src/vulkan/ClassicRaster.*`: The original VDB View uses mostly simple legacy OpenGL and basic shading to implement all of its rendering modules. As a result, there isn't a lot of complexity that needs to be captured in the Vulkan port. It was possible to combine the needs of all render modules into a simple monolithic render engine.
    * `VulkanClassicRasterEngine` implements functions for generating and recording Vulkan rendering commands which reproduce the different drawing and shading modes of the OpenGL original. Whereas the OpenGL VDB View splits these modes across several inline GLSL shaders and render modules, `VulkanClassicRasterEngine` provides a mechanism for drawing more generically, and implements the necessary shading modes in a single GLSL vertex/fragment shader pair (`src/glsl/standard.*`). 
        * The render engine class is a implemented as a global singleton with a nearly static lifetime. Vulkan render modules in `RenderModules.cc` utilize the singleton instance to configure and record the graphics commands that render the module's geometry. 
        * The 'standard' GLSL shader pipeline used by this class is mostly the same as the original shaders, but with updates to the Vulkan style. Vulkan _push-constants_ are used to switch between shading behaviors at runtime.
    * These sources also define `VulkanClassicRasterGeo` and its builder class `VulkanClassicRasterGeoBuilder`. This class acts as a very minimal format for specifying the geometry to be drawn by `VulkanClassicRasterEngine`. 
        * Each instance contains a Vulkan vertex buffer, index buffer, and an optional list of _parts_, splitting the buffer into sub-ranges which can be drawn individually or all together.
        * The builder class exists to simplify setup of `VulkanClassicRasterGeo`, accumulating vertex, index, and part data into `std::vectors`, then handling their upload into GPU memory as Vulkan buffers with a simple `build()` function.
* `src/vulkan/BitmapFont.*`: VDB View relies on legacy OpenGL bitmap and call list features to implement bitmap font rendering for the UI. Vulkan does not support any analogue of these features, so this header/source pair implements bitmap font rendering from the ground up.
    * Similar to `VulkanClassicRasterEngine`, a `VulkanBitmapFont13Engine` singleton is defined which can be used to generate and record graphics commands that render text. Font rendering occurs on the GPU during shading. 
    * `Font.h` is included `vulkan/BitmapFont.cc` to gain access to the `BitmapFont13::sCharacters` bitmap LUT.  
    * Lines of text to be drawn are enqueued by calling `addLine(..)`, specifying the string to rasterize and pixel coordinates at which to anchor the line. After lines are added, rendering commands can be recorded. All lines get batched together into a single draw call.
    * Bitmap font rendering occurs entirely in the vertex and fragment shaders. The methods used aren't necessarily the most performant or idiomatic, but font rendering isn't performance critical for VDB view. Details on the method used can be found in comments on the sources and shader code.

### Additional Design Notes
* Compromises have been made throughout Vulkan VDB View to minimize the scope of changes made to the original source. The original application's design matches well with OpenGL's global state machine and legacy functionality, and likewise is at odds with Vulkan's explicit design and lack of global state. Were VDB View written from the ground up with Vulkan in mind, it certainly would end up structured quite differently. Vulkan code and control flow outside of `src/vulkan/` should not necessarily be considered a reliable point of reference for Vulkan best practice. 
* To avoid the high development cost and verbosity of Vulkan's original core API, Vulkan VDB View leans heavily on the most recent Vulkan features and extensions to simplify development. In particular:
    * Dynamic rendering & synchronization2, now part of Vulkan 1.3's official (but optionally supported) feature set, are both required. Dynamic rendering vastly simplifies the process of setting up render passes, and the ability to suspend and resume render passes pairs extremely well with VDB View's render module design. 
    * `VK_KHR_push_descriptor`: Push descriptors are required as they greatly simplify the process of binding of resources to shaders.
    * `VK_EXT_vertex_input_dynamic_state`: Like the others, this extensions greatly reduces the amount of code required to configure vertex inputs before drawing in Vulkan.
    * `VK_EXT_shader_object`: Shader objects are one of the newest additions to the API, but their impact is massive. Shader objects massively simplify the process of creating, managing, and using shaders in Vulkan. Potentially cutting multiple hundreds of lines from something like VK VDB View. Unfortunately, by virtue of being so new, support is far from universal. See [Runtime Dependencies](#runtime-dependencies).
* Vulkan VDB View makes no attempts to leverage Vulkan's support for multi-threading and async, as this would be incompatible with the original and is simply unnecessary. 

## Building and Dependencies 
As long as the required dependencies are installed and visible to CMake, Vulkan VDB View should build just by enabling the `OPENVDB_BUILD_VK_VDB_VIEW` CMake option. 

### Dependencies 
In addition to the dependencies of the surrounding OpenVDB library, Vulkan VDB View depends on:
#### Build Dependencies
* **Vulkan 1.3.280:** (All are packaged with the VulkanSDK)
    * Vulkan C and C++ headers (vulkan.h and vulkan.hpp)
    * Vulkan Memory Allocator (VMA)
    * GLSL compiler executable (glslc)
* OpenGL
* GLFW 3
#### Runtime Dependencies 
* Up to date Vulkan loader. GLFW will require certain instance-level extensions depending on the platform, and your Vulkan installation must support them for Vulkan VDB View to work. 
* Vulkan compatible GPU with up to date driver. Additionally your device must support:
    * **Extensions:**
        * VK_KHR_swapchain: _required_
        * VK_EXT_vertex_input_dynamic_state: _required_
        * VK_KHR_push_descriptor: _required_
        * VK_EXT_shader_object: _required (emulation ok)_
    * **Physical Device Features:**
        * fullDrawIndexUint32: _required_
        * fillModeNonSolid: _required_
        * wideLines: _required_
        * largePoints: _required_
        * **VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT:**
            * vertexInputDynamicState: _required_
        * **VkPhysicalDeviceVulkan12Features:**
            * separateDepthStencilLayouts: _required_
            * scalarBlockLayout: _required_
            * shaderInt8: _required_
            * storageBuffer8BitAccess: _required_
            * uniformAndStorageBuffer8BitAccess: _required_
        * **VkPhysicalDeviceVulkan13Features:**
            * synchronization2: _required_
            * dynamicRendering: _required_
            * maintenance4: _required_
    * **Physical Device Properties:**
        * pointSizeRange ⊇ [1, 4]
    * 1 Queue supporting graphics, compute, transfer, and presentation operations

### Compatibility Notes
Vulkan VDB View was originally developed to run on Enterprise Linux based systems with current NVIDIA hardware and up to date graphics drivers. Limited testing has been done on other platforms, so results will likely vary with other hardware and software. Vulkan in general is not supported on Macs. No attempt has been made to get Vulkan VDB View building on MacOS or running on [MoltenVK](https://github.com/KhronosGroup/MoltenVK). It seems certain that Vulkan VDB View would not work with MoltenVK unless significantly modified. 

The `VK_EXT_shader_objects` extension was hugely helpful in reducing the development effort of Vulkan VDB View. Unfortunately, the extension is new and not yet widely supported. Even on leading edge desktop hardware, very recent drivers are necessary to gain support for this extension. Fortunately the extension can also be supported through an emulation layer, which now ships with the VulkanSDK. Vulkan VDB View looks for this emulation layer during setup, and enables it automatically if found. It is also possible to install the layer separately, or even ship it alongside builds, but this is beyond the scope of this project, and there are plenty of other compatibility hazards to contend with when it comes to shader object support. 