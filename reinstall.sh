#!/usr/bin/bash
cd ~/excalibar
cd lib
sudo make uninstall
cd ../bar
sudo make uninstall

cd ~/excalibar
cd lib
make
sudo make install
cd ../bar
make
sudo make install
make cpconfig
cd ../plugins
make
make install
