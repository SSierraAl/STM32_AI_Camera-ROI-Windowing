import cv2
import time
import sys
import numpy as np

# ========================= CONFIGURATION =========================
CAMERA_INDEX = 0          # Change this if your camera is not at index 0
FRAME_WIDTH = 640         # Desired width
FRAME_HEIGHT = 480        # Desired height
FPS = 30                  # Desired frame rate






# Use DirectShow backend - better for UVC cameras on Windows
CAP_BACKEND = cv2.CAP_DSHOW

# Try to force RGB format (some cameras support this)
# FourCC codes: 'RGB3' = 24-bit RGB, 'YUY2' = YUV 4:2:2
TARGET_FOURCC = cv2.VideoWriter_fourcc(*'RGB3')  # Try to get RGB output

# Additional camera properties for stability
SET_BUFFER_SIZE = 1

# =================================================================

def print_camera_info(cap):
    """Print detailed camera information"""
    print("\n" + "=" * 60)
    print("DETAILED CAMERA INFORMATION")
    print("=" * 60)
    
    print(f"\nBackend: {cap.getBackendName()}")
    
    print(f"\n--- Property Values ---")
    props_to_check = [
        (cv2.CAP_PROP_FRAME_WIDTH, "Frame Width"),
        (cv2.CAP_PROP_FRAME_HEIGHT, "Frame Height"),
        (cv2.CAP_PROP_FPS, "FPS"),
        (cv2.CAP_PROP_FOURCC, "FourCC Codec"),
        (cv2.CAP_PROP_BUFFERSIZE, "Buffer Size"),
    ]
    
    for prop_id, prop_name in props_to_check:
        try:
            value = cap.get(prop_id)
            if value != 0 and value != -1:
                print(f"  {prop_name:25s}: {value}")
        except:
            pass
    
    # Try to get FourCC codec
    fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    if fourcc > 0:
        codec = chr(fourcc & 0xff) + chr((fourcc >> 8) & 0xff) + chr((fourcc >> 16) & 0xff) + chr((fourcc >> 24) & 0xff)
        print(f"\n  FourCC Codec: {codec} (0x{fourcc:08X})")
    
    print("\n" + "=" * 60)

def yuy2_to_rgb(frame):
    """
    Convert YUY2 (YUV 4:2:2) frame to RGB.
    YUY2 has 2 bytes per pixel: Y0 U Y1 V pattern
    Output is RGB888 (3 bytes per pixel)
    """
    h, w = frame.shape[:2]
    
    # Reshape to get Y and U/V components
    # YUY2 format: Y0 U Y1 V Y0 U Y1 V ...
    yuy2 = frame.reshape(-1)
    
    # Extract Y, U, V components
    y0 = yuy2[0::4].astype(np.int16)  # Y for even pixels
    u = yuy2[1::4].astype(np.int16)   # U (Cb) - shared between pairs
    y1 = yuy2[2::4].astype(np.int16)  # Y for odd pixels
    v = yuy2[3::4].astype(np.int16)   # V (Cr) - shared between pairs
    
    # Expand U and V to match Y length
    u_expanded = np.repeat(u, 2)
    v_expanded = np.repeat(v, 2)
    
    # YUV to RGB conversion (BT.601)
    # R = Y + 1.402 * (V - 128)
    # G = Y - 0.344 * (U - 128) - 0.714 * (V - 128)
    # B = Y + 1.772 * (U - 128)
    
    u_centered = u_expanded - 128
    v_centered = v_expanded - 128
    
    r = y0 + np.round(1.402 * v_centered)
    g = y0 - np.round(0.344 * u_centered) - np.round(0.714 * v_centered)
    b = y0 + np.round(1.772 * u_centered)
    
    # Clip to valid range
    r = np.clip(r, 0, 255).astype(np.uint8)
    g = np.clip(g, 0, 255).astype(np.uint8)
    b = np.clip(b, 0, 255).astype(np.uint8)
    
    # Interleave R, G, B
    rgb = np.stack([r, g, b], axis=-1).reshape(h, w, 3)
    
    return rgb

def analyze_frame(frame, frame_num, prev_frame=None):
    """Analyze frame for common issues"""
    if frame is None:
        return {"error": "Frame is None"}
    
    analysis = {
        "frame_num": frame_num,
        "shape": frame.shape,
        "dtype": str(frame.dtype),
        "min": int(np.min(frame)),
        "max": int(np.max(frame)),
        "mean": float(np.mean(frame)),
        "std": float(np.std(frame)),
    }
    
    if len(frame.shape) == 3:
        h, w, c = frame.shape
        analysis["channels"] = c
        
        # Check for YUY2 artifacts (duplicate columns indicate YUY2 being misinterpreted)
        if c == 3:
            # Check if adjacent columns are identical (YUY2 artifact)
            col_diff = np.abs(frame[:, :-1, :].astype(int) - frame[:, 1:, :].astype(int)).mean()
            if col_diff < 5:
                analysis["warning"] = "Possible YUY2 artifact detected - columns are too similar"
        
        # Only flag as frozen if std is extremely low (near zero variance)
        # A static scene is NOT frozen - it's normal behavior
        if analysis["std"] < 1.0:
            analysis["warning"] = "VERY LOW VARIANCE - Image may be frozen!"
        
        row_means = np.mean(frame, axis=1)
        row_var = np.var(row_means)
        if row_var > 1000:
            analysis["warning"] = "BANDING DETECTED - Row variance is high"
    
    # Compare with previous frame if available
    if prev_frame is not None and prev_frame.shape == frame.shape:
        diff = np.abs(frame.astype(int) - prev_frame.astype(int)).mean()
        analysis["frame_diff"] = float(diff)
        if diff < 0.5:
            analysis["warning"] = "Frame identical to previous - camera may be frozen"
    
    return analysis

def list_available_cameras():
    """List all available cameras"""
    print("\n" + "=" * 60)
    print("SCANNING FOR AVAILABLE CAMERAS")
    print("=" * 60)
    
    available = []
    for i in range(10):
        cap = cv2.VideoCapture(i, CAP_BACKEND)
        if cap.isOpened():
            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            fps = cap.get(cv2.CAP_PROP_FPS)
            fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
            if fourcc > 0:
                codec = chr(fourcc & 0xff) + chr((fourcc >> 8) & 0xff) + chr((fourcc >> 16) & 0xff) + chr((fourcc >> 24) & 0xff)
            else:
                codec = "UNKNOWN"
            print(f"  Camera {i}: {width}x{height} @ {fps}fps - AVAILABLE ({codec})")
            available.append(i)
            cap.release()
        else:
            print(f"  Camera {i}: NOT AVAILABLE")
    
    print("=" * 60)
    return available

def open_camera_with_retry(camera_index, width, height, fps, backend, max_retries=5):
    """
    Open camera with retry logic and proper configuration.
    Returns cap object and success status.
    """
    print(f"\nOpening camera at index {camera_index}...")
    print(f"Requested: {width}x{height} @ {fps}fps")
    print(f"Backend: {backend}")
    
    cap = None
    
    for attempt in range(max_retries):
        print(f"\n--- Attempt {attempt + 1}/{max_retries} ---")
        
        cap = cv2.VideoCapture(camera_index, backend)
        
        if not cap.isOpened():
            print(f"Could not open camera")
            if attempt < max_retries - 1:
                print(f"Retrying in 1 second...")
                time.sleep(1)
            continue
        
        # Try to set FourCC to RGB first
        print(f"Attempting to set FourCC to RGB3...")
        cap.set(cv2.CAP_PROP_FOURCC, TARGET_FOURCC)
        
        # Set other properties
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        cap.set(cv2.CAP_PROP_FPS, fps)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, SET_BUFFER_SIZE)
        
        # Verify settings
        actual_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        actual_fps = cap.get(cv2.CAP_PROP_FPS)
        actual_fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
        
        if actual_fourcc > 0:
            codec = chr(actual_fourcc & 0xff) + chr((actual_fourcc >> 8) & 0xff) + chr((actual_fourcc >> 16) & 0xff) + chr((actual_fourcc >> 24) & 0xff)
            print(f"Actual FourCC: {codec}")
        
        print(f"Camera opened: {cap.getBackendName()}")
        print(f"Resolution: {actual_width} x {actual_height}")
        print(f"FPS: {actual_fps}")
        
        # Check if we got YUY2 format (common for UVC cameras)
        if actual_fourcc > 0:
            codec = chr(actual_fourcc & 0xff) + chr((actual_fourcc >> 8) & 0xff) + chr((actual_fourcc >> 16) & 0xff) + chr((actual_fourcc >> 24) & 0xff)
            if codec == "YUY2":
                print("\n*** Camera is using YUY2 format ***")
                print("Will convert to RGB in software for proper display")
        
        # Drain any buffered frames
        print("Draining old frames...")
        for i in range(10):
            cap.grab()
        
        # Test multiple frames
        test_frames_ok = 0
        for i in range(5):
            test_ret, test_frame = cap.read()
            if test_ret and test_frame is not None:
                test_frames_ok += 1
                print(f"Test frame {i+1}: shape={test_frame.shape}, dtype={test_frame.dtype}, "
                      f"pixel_range=[{np.min(test_frame)}, {np.max(test_frame)}]")
            else:
                print(f"Test frame {i+1}: FAILED (ret={test_ret})")
        
        if test_frames_ok >= 3:
            print(f"Success! {test_frames_ok}/5 test frames captured")
            print_camera_info(cap)
            return cap, True
        else:
            print(f"Warning: Only {test_frames_ok}/5 test frames succeeded")
            cap.release()
            if attempt < max_retries - 1:
                time.sleep(1)
    
    print(f"\nFailed to open camera after {max_retries} attempts")
    return None, False

def main():
    print("=" * 60)
    print("UVC Camera Test Program - FIXED COLOR FORMAT")
    print("=" * 60)
    print(f"\nPython version: {sys.version.split()[0]}")
    print(f"OpenCV version: {cv2.__version__}")
    print(f"NumPy version: {np.__version__}")
    
    # List available cameras first
    available_cameras = list_available_cameras()
    
    if CAMERA_INDEX not in available_cameras:
        print(f"\nWARNING: Camera {CAMERA_INDEX} not found in available cameras!")
    
    # Open camera with retry
    cap, success = open_camera_with_retry(
        CAMERA_INDEX, FRAME_WIDTH, FRAME_HEIGHT, FPS, CAP_BACKEND
    )
    
    if not success:
        print("\nERROR: Could not open camera!")
        print("\nTroubleshooting steps:")
        print("1. Check if camera is properly connected")
        print("2. Try a different USB port")
        print("3. Close other applications using the camera")
        print("4. Try changing CAMERA_INDEX to 1, 2, 3...")
        sys.exit(1)
    
    # Check if we need YUY2 conversion
    actual_fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    needs_yuy2_conversion = False
    if actual_fourcc > 0:
        codec = chr(actual_fourcc & 0xff) + chr((actual_fourcc >> 8) & 0xff) + chr((actual_fourcc >> 16) & 0xff) + chr((actual_fourcc >> 24) & 0xff)
        if codec == "YUY2":
            needs_yuy2_conversion = True
            print("\n*** Enabling YUY2 to RGB conversion ***")
    
    print("\n" + "=" * 60)
    print("Camera is ready! Press 'q' to quit, 'd' for debug info")
    print("=" * 60)
    
    frame_count = 0
    start_time = time.time()
    last_analysis = None
    consecutive_same_frame = 0
    error_count = 0
    
    try:
        while True:
            ret, frame = cap.read()
            
            if not ret:
                error_count += 1
                print(f"\nERROR {error_count}: Failed to grab frame!")
                
                if error_count >= 5:
                    print("Too many errors, attempting recovery...")
                    cap.release()
                    time.sleep(1)
                    cap, success = open_camera_with_retry(
                        CAMERA_INDEX, FRAME_WIDTH, FRAME_HEIGHT, FPS, CAP_BACKEND
                    )
                    if not success:
                        print("Could not recover camera connection")
                        break
                    error_count = 0
                continue
            
            if frame is None:
                print("ERROR: Frame is None!")
                continue
            
            # Note: OpenCV with DirectShow typically converts YUY2 to BGR automatically
            # If the image looks wrong, the camera may be outputting in a format
            # that OpenCV cannot properly decode. In that case, try:
            # 1. Using a different backend (CAP_ANY instead of CAP_DSHOW)
            # 2. Setting the FourCC to a specific format before opening
            
            error_count = 0
            frame_count += 1
            
            if frame_count % 10 == 0:
                analysis = analyze_frame(frame, frame_count)
                
                if last_analysis is not None and "shape" in analysis and "shape" in last_analysis:
                    if analysis["shape"] == last_analysis["shape"]:
                        if "std" in analysis and "std" in last_analysis:
                            if abs(analysis["std"] - last_analysis["std"]) < 1:
                                consecutive_same_frame += 1
                            else:
                                consecutive_same_frame = 0
                        else:
                            consecutive_same_frame = 0
                    else:
                        consecutive_same_frame = 0
                else:
                    consecutive_same_frame = 0
                
                if consecutive_same_frame >= 3:
                    print(f"\n*** WARNING: Frame appears frozen! ***")
                
                last_analysis = analysis
                
                print(f"\n--- Frame {frame_count} Analysis ---")
                print(f"Shape: {analysis.get('shape', 'N/A')}")
                print(f"Dtype: {analysis.get('dtype', 'N/A')}")
                print(f"Pixel range: {analysis.get('min', 'N/A')} - {analysis.get('max', 'N/A')}")
                print(f"Mean: {analysis.get('mean', 'N/A'):.2f}, Std: {analysis.get('std', 'N/A'):.2f}")
                if "warning" in analysis:
                    print(f"*** {analysis['warning']} ***")
                
                elapsed = time.time() - start_time
                fps = frame_count / elapsed
                print(f"Current FPS: {fps:.1f} | Errors: {error_count}")
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                print("\nQuitting...")
                break
            elif key == ord('d'):
                print("\n--- Manual Debug Dump ---")
                print(f"Frame shape: {frame.shape}")
                print(f"Frame dtype: {frame.dtype}")
                print(f"First 3x3 pixels:")
                print(frame[:3, :3])
            
            cv2.imshow('UVC Camera - Press Q to quit, D for debug', frame)
                
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"\nException occurred: {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()
    
    finally:
        print("\nReleasing camera resources...")
        cap.release()
        cv2.destroyAllWindows()
        
        elapsed = time.time() - start_time
        if elapsed > 0:
            final_fps = frame_count / elapsed
            print(f"\n{'=' * 60}")
            print("SESSION STATISTICS")
            print(f"{'=' * 60}")
            print(f"  Total frames captured: {frame_count}")
            print(f"  Session duration: {elapsed:.2f} seconds")
            print(f"  Average FPS: {final_fps:.2f}")
            print(f"  Total errors: {error_count}")
            print(f"{'=' * 60}")
        
        print("Camera released. Goodbye!")

if __name__ == "__main__":
    main()