#!/usr/bin/bash
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
