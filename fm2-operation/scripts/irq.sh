# for d in /proc/irq/*/ ;
# do
# 	echo ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff > $d/smp_affinity
# done

# echo ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff,ffffffff > /proc/irq/default_smp_affinity

source config.txt

echo $NIC_NAME

sudo ethtool -C $NIC_NAME adaptive-rx off adaptive-tx off 
sudo ethtool -C $NIC_NAME adaptive-rx off adaptive-tx off
sudo ethtool -C $NIC_NAME rx-usecs 0 tx-usecs 0 
sudo ethtool -C $NIC_NAME rx-usecs 0 tx-usecs 0
sudo ethtool -C $NIC_NAME rx-frames-irq 1 tx-frames-irq 1
sudo ethtool -C $NIC_NAME rx-frames-irq 1 tx-frames-irq 1
sudo ethtool -C $NIC_NAME rx-frames 1 tx-frames 1
sudo ethtool -C $NIC_NAME rx-frames 1 tx-frames 1
sudo ethtool -L $NIC_NAME combined 1

sudo systemctl stop irqbalance
sudo systemctl disable irqbalance

pushd /sys/class/net/$NIC_NAME/device/msi_irqs/

for IRQ in *; do
	sudo echo $NIC_INTERRUPT_CORE | sudo tee /proc/irq/$IRQ/smp_affinity_list
done

popd