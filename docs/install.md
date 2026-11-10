# Install the driver

## Install the driver on Linux

You can install the driver using the debian package with

> sudo apt install ./vscpl2drv-ikea-dirigera_x.y.z.deb

the driver will be installed to */var/lib/vscp/drivers/level2*. The test
tool is installed as *vscpl2drv-ikea-dirigera-test* and a sample
configuration file is placed in */usr/share/vscpl2drv-ikea-dirigera/*.

Packages for x64 and Raspberry Pi (32- and 64-bit) are published for every
release on the project's
[GitHub Releases](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/releases)
page.

If you want to use the driver with the
[VSCP Daemon](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_daemon)
you need to add it to the vscpd.json file (/etc/vscp/vscpd.json). See
[this document](https://grodansparadis.github.io/vscp/#/configuring_the_vscp_daemon)
on how to do this.

You also need to set up a configuration file for the driver itself - see
[Configure the driver](./configure.md). If you don't need to dynamically
edit the content of this file a good and safe location for it is in the
*/etc/vscp/* folder alongside the VSCP daemon configuration file.

Before the driver can talk to the hub you must pair with it once to obtain
an access token - see [Test and use the driver](./using.md#pairing).

## Install the driver on Windows

Download the Windows package from the project's
[GitHub Releases](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/releases)
page. Packages include an NSIS installer (`.exe`) and a portable ZIP
archive.

- Run the installer and follow its prompts, or extract the ZIP to a folder
  accessible to the VSCP host application.
- Keep the driver DLL and its runtime dependency DLLs together in the
  installed folder, or ensure those dependencies are available on `PATH`.
- Create a driver configuration JSON file - see
  [Configure the driver](./configure.md).
- Add a Level II driver entry to the VSCP daemon or other host application,
  pointing `path-driver` to the installed driver DLL and `path-config` to
  the driver configuration file.

If no Windows binary package is available for your release, build the
driver from source using the [build instructions](./build.md).

## Install the driver on MacOS

Download the macOS package (`.tgz`/`.zip`) from the project's
[GitHub Releases](https://github.com/grodansparadis/vscpl2drv-ikea-dirigera/releases)
page, if one is published for your macOS version and CPU architecture.

Extract the archive, then copy the driver library (`.dylib`) to a directory
readable by the VSCP host application. If the release does not include a
macOS package, build the library using the
[build instructions](./build.md).

The driver depends on libcurl, libwebsockets, OpenSSL and SQLite3. If these
libraries are not bundled with the release, install compatible runtime
libraries for your architecture (for example, with Homebrew) and ensure the
host process can find them.

Create a driver configuration file, then configure your VSCP host
application to load the `.dylib` and pass the driver configuration path.
See [Configure the driver](./configure.md) and the host application's
documentation.

[filename](./bottom-copyright.md ':include')
