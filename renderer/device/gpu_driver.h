#ifndef RENDERER_DEVICE_GPU_DRIVER_H_
#define RENDERER_DEVICE_GPU_DRIVER_H_

namespace renderer {

// Which GPU family this process ended up on, and the driver workarounds that key off it.
//
// The one-frame region loss is not a property of the TV. Every symptom points at the Vulkan path of
// the Mali driver branch that TV happens to run (Mali-G52, driver 32.1.0 / ARM r32p1) - a branch
// that ships on a whole generation of G52/G76/G57 devices - while the GLES path of that same driver
// never shows it (both verified on the same machine). A workaround gated on "is this an Android TV"
// would therefore fix nothing outside one model, and would be wrong on a TV that is not Mali; the
// gate that generalises names the GPU family AND the Vulkan backend, so it fires only on Mali + Vulkan
// and stays off on Mali + GLES (where the defect does not exist).
//
// Kept deliberately identical in shape to the earlier device-mode flag: one process-wide boolean,
// set once at device creation from the adapter Diligent actually picked (render_device.cc), read
// wherever a workaround needs it. It is a plain flag rather than a query so that the read sites do
// not have to hold a render device pointer.
//
// Driver *version* is not part of the gate yet, on purpose: Diligent's GraphicsAdapterInfo carries
// the description, the vendor and the PCI ids, but not the driver version - the "Driver version
// 32.1.0" line in the log comes straight from the Vulkan physical device properties. A version
// range would mean querying Vulkan directly, so add it when there is a newer Mali driver to compare
// against, and then keep the range here rather than at the call sites.
inline bool& MaliGpuFlag() {
  static bool enabled = false;
  return enabled;
}

inline void SetMaliGpu(bool enabled) {
  MaliGpuFlag() = enabled;
}

inline bool IsMaliGpu() {
  return MaliGpuFlag();
}

}  // namespace renderer

#endif  //! RENDERER_DEVICE_GPU_DRIVER_H_
