import esptool
print("Testing esptool import")
args = ['--chip', 'esp32', 'flash_id']
esptool.main(args)
