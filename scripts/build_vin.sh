BSP=/home/radxa/camwork/bsp150_patched; S=$BSP/drivers/vin; K=/lib/modules/$(uname -r)/build
cd $S
export KCFLAGS="-I$BSP/include -I$BSP/drivers/vin -I$BSP/drivers/vin/utility -I$BSP/drivers/vin/vin-cci -I$BSP/drivers/vin/platform -I$BSP/drivers/vin/vin-video -I$BSP/drivers/vin/vin-mipi -I$BSP/drivers/vin/vin-csi -I$BSP/drivers/vin/vin-isp -I$BSP/drivers/vin/vin-vipp -I$BSP/drivers/vin/vin-stat -I$BSP/drivers/vin/vin-tdm -I$BSP/drivers/vin/modules/sensor -I$BSP/drivers/vin/modules/actuator -I$BSP/drivers/vin/modules/flash -DDYNAMIC_DEBUG_MODULE -Wno-error"
make -C $K M=$S KBUILD_MODPOST_WARN=1 modules > /home/radxa/camwork/build_vin.log 2>&1
echo "make exit=$?" > /home/radxa/camwork/build_vin.done
