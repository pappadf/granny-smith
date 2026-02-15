# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in Granny Smith, please report it responsibly.

### How to Report

1. **Do NOT** open a public GitHub issue for security vulnerabilities
2. Use [GitHub's private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability) to submit your report
3. Include as much detail as possible to help us understand and reproduce the issue

### What to Include

- **Description**: A clear description of the vulnerability
- **Impact**: What an attacker could achieve by exploiting it
- **Reproduction Steps**: Step-by-step instructions to reproduce the issue
- **Environment**: Browser, OS, and any relevant configuration
- **Proposed Fix**: If you have suggestions for fixing the issue

### Response Process

- **Acknowledgment**: We will acknowledge receipt of your report within 48 hours
- **Investigation**: We will investigate and provide an initial assessment within 1 week
- **Resolution**: We will work to resolve the issue as quickly as possible
- **Disclosure**: We will coordinate with you on appropriate disclosure timing

### Security Considerations

Granny Smith runs as a WebAssembly application in the browser. Key security areas include:

- **Input validation**: ROM images, disk images, and archive files are parsed from potentially untrusted sources
- **Cross-origin isolation**: The emulator requires COOP/COEP headers for SharedArrayBuffer; serve only from a properly configured server
- **Emulated network**: The built-in AFP file server exposes browser-side files to the emulated guest OS — be mindful of what files are shared
