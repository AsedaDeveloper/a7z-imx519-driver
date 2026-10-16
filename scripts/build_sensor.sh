set -e
BSP=/home/radxa/camwork/bsp150; S=$BSP/drivers/vin/modules/sensor; K=/lib/modules/$(uname -r)/build
cd $S
[ -f Makefile.orig ] || cp Makefile Makefile.orig
echo 'obj-$(CONFIG_SENSOR_IMX519_MIPI) += imx519_mipi.o' > Makefile
cp /home/radxa/camwork/imx519_mipi.c $S/imx519_mipi.c
rm -f *.o *.ko .*.cmd Module.symvers modules.order 2>/dev/null || true
export KCFLAGS="-I$BSP/include -I$BSP/drivers/vin -I$BSP/drivers/vin/utility -I$BSP/drivers/vin/modules/sensor -I$BSP/drivers/vin/vin-cci -I$BSP/drivers/vin/platform -DDYNAMIC_DEBUG_MODULE -Wall -Wno-error"
set +e
make -C $K M=$S CONFIG_SENSOR_IMX519_MIPI=m KBUILD_MODPOST_WARN=1 modules > /home/radxa/camwork/build519.log 2>&1
echo "make exit=$?"
echo "--- warnings/errors:"; grep -E "error|warning" /home/radxa/camwork/build519.log | sed "s#$S/##" | head -30 | cut -c1-200
ls -la $S/imx519_mipi.ko 2>&1
