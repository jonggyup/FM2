if [[ "$1" != "always" && "$1" != "never" ]]; then
    echo "Usage: $0 [always|never]"
    exit 1
fi

sudo sh -c "echo $1 | tee /sys/kernel/mm/transparent_hugepage/enabled"
sudo sh -c "echo $1 | tee /sys/kernel/mm/transparent_hugepage/defrag"