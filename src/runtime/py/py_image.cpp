/*
 * py_image.cpp — Python bindings for the image kernels (runtime_py.image).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release. Bytes are extracted with
 * PyBytes_AsStringAndSize BEFORE the release and never touched while the GIL
 * is released.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/image/image_sniff.h>

namespace py = pybind11;

namespace {

// Extract a string_view over a py::bytes without copying. The view is valid
// as long as the py::bytes object is alive.
bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

} // namespace

void py_register_image(py::module_& m) {
    m.doc() = "Image kernels: header dimension sniffing, EXIF orientation, animated WebP.";

    m.def(
        "sniff_dimensions",
        [](py::bytes data) -> py::object {
            kimix::string_view view;
            if (!bytes_view(data, view)) {
                throw py::error_already_set();
            }
            kimix::optional<kimix::runtime::image::dimensions> result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = kimix::runtime::image::sniff_dimensions(view);
            }
            if (!result) {
                return py::none();
            }
            return py::make_tuple(result->width, result->height, result->transposed);
        },
        "Return (width, height, transposed) or None. transposed is True when "
        "JPEG EXIF orientation 5-8 swaps width/height.",
        py::arg("data"));

    m.def(
        "read_exif_orientation",
        [](py::bytes data) -> py::object {
            kimix::string_view view;
            if (!bytes_view(data, view)) {
                throw py::error_already_set();
            }
            kimix::optional<int> result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = kimix::runtime::image::read_exif_orientation(view);
            }
            if (!result) {
                return py::none();
            }
            return py::int_(*result);
        },
        "Return 1-8 or None.",
        py::arg("data"));

    m.def(
        "is_animated_webp",
        [](py::bytes data) -> bool {
            kimix::string_view view;
            if (!bytes_view(data, view)) {
                throw py::error_already_set();
            }
            bool result = false;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = kimix::runtime::image::is_animated_webp(view);
            }
            return result;
        },
        "True for animated VP8X WebP.",
        py::arg("data"));

    m.def(
        "format_byte_size",
        [](uint64_t n) -> py::str {
            kimix::string result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = kimix::runtime::image::format_byte_size(n);
            }
            return py::str(result.data(), result.size());
        },
        "Human-readable byte size matching image_compress.py.",
        py::arg("n"));
}
