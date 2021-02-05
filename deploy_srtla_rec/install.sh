cp ../srtla_rec /usr/local/bin/srtla_rec
chown root:root /usr/local/bin/srtla_rec
chmod 755 /usr/local/bin/srtla_rec

# don't replace if it already exists as it holds user settings
cp -n srtla_rec_default /etc/default/srtla_rec
chown root:root /etc/default/srtla_rec
chmod 644 /etc/default/srtla_rec

cp srtla_rec.service /etc/systemd/system/
chown root:root /etc/systemd/system/srtla_rec.service
chmod 644 /etc/systemd/system/srtla_rec.service
