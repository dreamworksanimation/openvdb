// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include <openvdb/openvdb.h>
#include "Viewer.h"
#include <boost/algorithm/string/classification.hpp> // for boost::is_any_of()
#include <boost/algorithm/string/predicate.hpp> // for boost::starts_with()
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


inline void
usage [[noreturn]] (const char* progName, int status)
{
    (status == EXIT_SUCCESS ? std::cout : std::cerr) <<
"Usage: " << progName << " file.vdb [file.vdb ...] [options]\n" <<
"Which: displays OpenVDB grids\n" <<
"Options:\n" <<
"    -i                                 print grid information\n" <<
"    -h, -help                          print this usage message and exit\n" <<
"    -version                           print version information\n" <<
"    -s, --msaa={1x, 2x, 4x, 8x, 16x}   Set multi-sampling anti-aliasing sample count. Default is 4x. 1x disables MSAA.\n" << 
"                                       Your system may not support all counts, in which case it will warn and fallback.\n" <<
"    --backend={opengl, vulkan}         set graphics backend\n" << 
"\n" <<
"Controls:\n" <<
"    Esc                exit\n" <<
"    -> (Right)         show next grid\n" <<
"    <- (Left)          show previous grid\n" <<
"    1                  toggle tree topology view on/off\n" <<
"    2                  toggle surface view on/off\n" <<
"    3                  toggle data view on/off\n" <<
"    G                  (\"geometry\") look at center of geometry\n" <<
"    H                  (\"home\") look at origin\n" <<
"    I                  toggle on-screen grid info on/off\n" <<
"    left mouse         tumble\n" <<
"    right mouse        pan\n" <<
"    mouse wheel        zoom\n" <<
"\n" <<
"    X + wheel          move right cut plane\n" <<
"    Shift + X + wheel  move left cut plane\n" <<
"    Y + wheel          move top cut plane\n" <<
"    Shift + Y + wheel  move bottom cut plane\n" <<
"    Z + wheel          move front cut plane\n" <<
"    Shift + Z + wheel  move back cut plane\n" <<
"    Ctrl + X + wheel   move both X cut planes\n" <<
"    Ctrl + Y + wheel   move both Y cut planes\n" <<
"    Ctrl + Z + wheel   move both Z cut planes\n";
    exit(status);
}


////////////////////////////////////////


int
main(int argc, char *argv[])
{
    const char* progName = argv[0];
    if (const char* ptr = ::strrchr(progName, '/')) progName = ptr + 1;

    int status = EXIT_SUCCESS;

    try {
        openvdb::initialize();
        openvdb::logging::initialize(argc, argv);

        bool printInfo = false, printGLInfo = false, printVersionInfo = false;

        openvdb_viewer::ViewerBackend backend = openvdb_viewer::ViewerBackend::eVulkan;
        uint32_t sampleCount = 4u;

        // Parse the command line.
        std::vector<std::string> filenames;
        for (int n = 1; n < argc; ++n) {
            std::string str(argv[n]);
            if (str[0] != '-') {
                filenames.push_back(str);
            } else if (str == "-i") {
                printInfo = true;
            } else if (str == "-d") { // deprecated
                printGLInfo = true;
            } else if (str == "-h" || str == "-help" || str == "--help") {
                usage(progName, EXIT_SUCCESS);
            } else if (str == "-version" || str == "--version") {
                printVersionInfo = true;
                printGLInfo = true;
            } else if (str == "-s" || str.substr(0, 6) == "--msaa"){
                std::string countStr;
                if (str != "-s" && str[6] == '=') {
                    countStr = str.substr(7, str.size());
                } else if (n+1 < argc) {
                    countStr = argv[++n];
                } else {
                    std::cerr << (str == "-s" ? "-s" : "--msaa") << " specified, but not argument provided. Should be one of "
                              << "(1x, 2x, 4x, 8x, 16x)" << std::endl;
                    usage(progName, EXIT_FAILURE);
                }

                try {
                    sampleCount = std::stoul(countStr.substr(0, countStr.size()-1));
                    const bool isPowerOf2 = (sampleCount & (sampleCount - 1)) == 0;
                    if (!isPowerOf2 || sampleCount < 0 || sampleCount > 16) {
                        std::cerr << "Invalid msaa sample count '" << countStr << "'. Must be one of (1x, 2x, 4x, 8x, 16x)" << std::endl;
                        usage(progName, EXIT_FAILURE);
                    }
                } catch (const std::logic_error& err) {
                    std::cerr << "Invalid msaa sample count '" << countStr << "'. Must be one of (1x, 2x, 4x, 8x, 16x)" << std::endl;
                    usage(progName, EXIT_FAILURE);
                }
            } else if (str.substr(0, 9) == "--backend") {
                std::string apiStr;

                if (str[9] == '=') {
                    apiStr = str.substr(10, str.size());
                } else if (n+1 < argc) {
                    apiStr = argv[++n];
                } else {
                    std::cerr << "--backend flagged, but no backend specified. Should be 'opengl' or 'vulkan'." << std::endl;
                    usage(progName, EXIT_FAILURE);
                }

                if (apiStr == "vulkan") {
                    backend = openvdb_viewer::ViewerBackend::eVulkan;
                } else if (apiStr == "opengl") {
                    backend = openvdb_viewer::ViewerBackend::eOpenGL;
                } else {
                    std::cerr << "Backend '" << apiStr << "' invalid. Must be 'opengl' or 'vulkan'.";
                    usage(progName, EXIT_FAILURE);
                }
            }
            else {
                usage(progName, EXIT_FAILURE);
            }
        }

        const size_t numFiles = filenames.size();

        if (printVersionInfo) {
            std::cout << "OpenVDB library version: "
                << openvdb::getLibraryAbiVersionString() << "\n";
            std::cout << "OpenVDB file format version: "
                << openvdb::OPENVDB_FILE_VERSION << std::endl;
            // If there are no files to view, don't print the OpenGL version,
            // since that would require opening a viewer window.
            if (numFiles == 0) return EXIT_SUCCESS;
        }
        if (numFiles == 0 && !printGLInfo) usage(progName, EXIT_FAILURE);

        openvdb_viewer::Viewer viewer = openvdb_viewer::init(progName, /*bg=*/false, backend);

        openvdb::GridCPtrVec allGrids;

        // Load VDB files.
        std::string indent(numFiles == 1 ? "" : "    ");
        for (size_t n = 0; n < numFiles; ++n) {
            openvdb::io::File file(filenames[n]);
            file.open();

            openvdb::GridPtrVecPtr grids = file.getGrids();
            if (grids->empty()) {
                OPENVDB_LOG_WARN(filenames[n] << " is empty");
                continue;
            }
            allGrids.insert(allGrids.end(), grids->begin(), grids->end());

            if (printInfo) {
                if (numFiles > 1) std::cout << filenames[n] << ":\n";
                for (size_t i = 0; i < grids->size(); ++i) {
                    const std::string name = (*grids)[i]->getName();
                    openvdb::Coord dim = (*grids)[i]->evalActiveVoxelDim();
                    std::cout << indent << (name.empty() ? "<unnamed>" : name)
                        << " (" << dim[0] << " x " << dim[1] << " x " << dim[2]
                        << " voxels)" << std::endl;
                }
            }
        }

        viewer.open(openvdb_viewer::DEFAULT_WIDTH, openvdb_viewer::DEFAULT_HEIGHT, sampleCount);

        if (printGLInfo) {
            // Now that the viewer window is open, we can get the OpenGL version, if requested.
            if (!printVersionInfo) {
                // Preserve the behavior of the deprecated -d option.
                std::cout << viewer.getVersionString() << std::endl;
            } else {
                // Print OpenGL and GLFW versions.
                std::ostringstream ostr;
                ostr << viewer.getVersionString(); // returns comma-separated list of versions
                const std::string s = ostr.str();
                std::vector<std::string> elems;
                boost::split(elems, s, boost::algorithm::is_any_of(","));
                for (size_t i = 0; i < elems.size(); ++i) {
                    boost::trim(elems[i]);
                    // Don't print the OpenVDB library version again.
                    if (!boost::starts_with(elems[i], "OpenVDB:")) {
                        std::cout << elems[i] << std::endl;
                    }
                }
            }
            if (numFiles == 0) return EXIT_SUCCESS;
        }

        viewer.view(allGrids);

        openvdb_viewer::exit();

    } catch (const char* s) {
        OPENVDB_LOG_FATAL(s);
        status = EXIT_FAILURE;
    } catch (std::exception& e) {
        OPENVDB_LOG_FATAL(e.what());
        status = EXIT_FAILURE;
    } catch (...) {
        OPENVDB_LOG_FATAL("Exception caught (unexpected type)");
        std::terminate();
    }

    return status;
}
