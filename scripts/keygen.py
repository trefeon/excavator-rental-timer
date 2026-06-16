import hmac
import hashlib
import sys

def generate_activation_code(device_id: str, package_type: str) -> str:
    """
    Generates the activation code for the RC Timer app.
    
    Args:
        device_id (str): The unique Device ID shown in the app.
        package_type (str): Either "PEMULA" (16 chars) or "CUAN" (20 chars).
        
    Returns:
        str: The generated activation code.
    """
    secret = "RC7T!m3r@K3y#2024$S3cr3t&S4f3Key".encode('utf-8')
    message = f"{device_id}|{package_type}".encode('utf-8')
    
    # Calculate HMAC-SHA256
    hmac_obj = hmac.new(secret, message, hashlib.sha256)
    hash_hex = hmac_obj.hexdigest().upper()
    
    # Truncate based on package type
    if package_type == "CUAN":
        return hash_hex[:20]
    elif package_type == "PEMULA":
        return hash_hex[:16]
    else:
        raise ValueError("Invalid package type. Must be 'PEMULA' or 'CUAN'.")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        device_id = sys.argv[1]
    else:
        device_id = input("Enter Device ID: ").strip()
        
    if not device_id:
        print("Device ID cannot be empty.")
        sys.exit(1)
        
    pemula_code = generate_activation_code(device_id, "PEMULA")
    cuan_code = generate_activation_code(device_id, "CUAN")
    
    print("\n--- Activation Codes ---")
    print(f"Device ID: {device_id}")
    print(f"Package PEMULA (16 chars): {pemula_code}")
    print(f"Package CUAN   (20 chars): {cuan_code}")
    print("------------------------")
