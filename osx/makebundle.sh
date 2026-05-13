#!/bin/sh

rm -rf PoxChat.app
rm -f *.app.zip

python $HOME/.local/bin/gtk-mac-bundler poxchat.bundle

echo "Compressing bundle"
#hdiutil create -format UDBZ -srcdir PoxChat.app -quiet PoxChat-2.9.6.1-$(git rev-parse --short master).dmg
zip -9rXq ./PoxChat-$(git describe --tags).app.zip ./PoxChat.app

