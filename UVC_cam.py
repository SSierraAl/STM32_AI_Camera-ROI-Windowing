import cv2
import time
import sys

# ========================= CONFIGURATION =========================
CAMERA_INDEX = 0          # Change this if your camera is not at index 0 (common values: 0, 1, 2)
FRAME_WIDTH = 640         # Desired width
FRAME_HEIGHT = 480        # Desired height
FPS = 30                  # Desired frame rate

# Backend selection - try different backends if default doesn't work
# CAP_BACKEND = cv2.CAP_ANY      # Default (auto) - tries all backends
# CAP_BACKEND = cv2.CAP_DSHOW    # Windows - DirectShow (good for many webcams)
# CAP_BACKEND = cv2.CAP_MSMF     # Windows - Media Foundation (modern Windows)
# CAP_BACKEND = cv2.CAP_V4L2     # Linux - Video4Linux2

CAP_BACKEND = cv2.CAP_ANY  # Auto-detect best backend

# Additional camera properties for stability
SET_BUFFER_SIZE = 1        # Minimize buffer for real-time streaming
AUTO_EXPOSURE = -1         # -1 = leave as is, 1 = manual, 3 = auto
EXPOSURE_VALUE = 100       # Exposure value (if manual)

# Timeout settings (in seconds)
CONNECTION_TIMEOUT = 5     # How long to wait for camera connection
FRAME_TIMEOUT = 2          # How long to wait for a frame before retry

# =================================================================

def open_camera_with_retry(camera_index, width, height, fps, backend, max_retries=5):
    """
    Open camera with retry logic and proper configuration.
    Returns cap object and success status.
    """
    print(f"Opening camera at index {camera_index}...")
    print(f"Requested: {width}x{height} @ {fps}fps")
    print(f"Backend: {backend}")
    
    cap = None
    
    for attempt in range(max_retries):
        # Open camera
        cap = cv2.VideoCapture(camera_index, backend)
        
        if not cap.isOpened():
            print(f"Attempt {attempt + 1}/{max_retries}: Could not open camera")
            if attempt < max_retries - 1:
                print(f"Retrying in 1 second...")
                time.sleep(1)
            continue
        
        # Set properties
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        cap.set(cv2.CAP_PROP_FPS, fps)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, SET_BUFFER_SIZE)
        
        # Try to set auto-exposure
        if AUTO_EXPOSURE != -1:
            cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, AUTO_EXPOSURE)
            if AUTO_EXPOSURE == 1:  # Manual
                cap.set(cv2.CAP_PROP_EXPOSURE, EXPOSURE_VALUE)
        
        # Verify settings
        actual_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        actual_fps = cap.get(cv2.CAP_PROP_FPS)
        
        print(f"Camera opened: {cap.getBackendName()}")
        print(f"Resolution: {actual_width} x {actual_height}")
        print(f"FPS: {actual_fps}")
        
        # Test frame grab
        test_ret, test_frame = cap.read()
        if test_ret and test_frame is not None:
            print(f"Test frame captured successfully: {test_frame.shape}")
            return cap, True
        else:
            print("Warning: Camera opened but test frame failed")
            cap.release()
            if attempt < max_retries - 1:
                time.sleep(1)
    
    print(f"Failed to open camera after {max_retries} attempts")
    return None, False

def main():
    print("=" * 50)
    print("UVC Camera Test Program")
    print("=" * 50)
    
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
        print("5. Try different backends (CAP_DSHOW, CAP_MSMF)")
        sys.exit(1)
    
    print("\n" + "=" * 50)
    print("Camera is ready! Press 'q' to quit")
    print("=" * 50)
    
    frame_count = 0
    start_time = time.time()
    
    try:
        while True:
            # Read frame with timeout
            ret, frame = cap.read()
            
            if not ret:
                print("Warning: Failed to grab frame, trying to recover...")
                # Try to recover by re-opening
                cap.release()
                cap, success = open_camera_with_retry(
                    CAMERA_INDEX, FRAME_WIDTH, FRAME_HEIGHT, FPS, CAP_BACKEND
                )
                if not success:
                    print("Could not recover camera connection")
                    break
                continue
            
            if frame is None:
                print("Warning: Frame is None")
                continue
            
            frame_count += 1
            
            # Calculate and display FPS
            if frame_count % 30 == 0:  # Every 30 frames
                elapsed = time.time() - start_time
                fps = frame_count / elapsed
                print(f"FPS: {fps:.1f} | Frames: {frame_count}")
            
            # Optional: Flip the frame horizontally (mirror effect)
            # frame = cv2.flip(frame, 1)
            
            # Display frame info
            cv2.putText(frame, f"UVC Camera - {FRAME_WIDTH}x{FRAME_HEIGHT}", 
                       (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
            cv2.putText(frame, f"Press 'q' to quit", 
                       (10, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
            
            # Display the frame
            cv2.imshow('UVC Camera - Press Q to quit', frame)
            
            # Press 'q' to exit (with 1ms wait for responsiveness)
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                print("\nQuitting...")
                break
                
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    
    finally:
        # Cleanup
        print("\nReleasing camera resources...")
        cap.release()
        cv2.destroyAllWindows()
        
        # Print final statistics
        elapsed = time.time() - start_time
        if elapsed > 0:
            final_fps = frame_count / elapsed
            print(f"\nSession Statistics:")
            print(f"  Total frames: {frame_count}")
            print(f"  Duration: {elapsed:.1f} seconds")
            print(f"  Average FPS: {final_fps:.1f}")
        
        print("Camera released. Goodbye!")

if __name__ == "__main__":
    main()