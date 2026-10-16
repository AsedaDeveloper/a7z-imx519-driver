set -e
BSP=/home/radxa/camwork/bsp150; S=$BSP/drivers/vin/modules/actuator; K=/lib/modules/$(uname -r)/build
cd $S; rm -f *.o *.ko .*.cmd Module.symvers modules.order 2>/dev/null || true
export KCFLAGS="-I$BSP/include -I$BSP/drivers/vin -I$BSP/drivers/vin/utility -I$BSP/drivers/vin/modules/actuator -I$BSP/drivers/vin/vin-cci -I$BSP/drivers/vin/platform -DCONFIG_ACTUATOR_MODULE=1 -DDYNAMIC_DEBUG_MODULE -Wall -Wno-error"
set +e
make -C $K M=$S CONFIG_ACTUATOR_AK7375=m KBUILD_MODPOST_WARN=1 modules > /home/radxa/camwork/build_act.log 2>&1
echo "make exit=$?"; grep -E "error" /home/radxa/camwork/build_act.log | head -10 | cut -c1-200
ls -la $S/ak7375_act.ko 2>&1
