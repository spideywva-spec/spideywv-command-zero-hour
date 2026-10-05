FROM node:22-alpine
WORKDIR /app
COPY server/online-api/package.json ./package.json
COPY server/online-api/server.js ./server.js
COPY server/online-api/test.js ./test.js
RUN mkdir -p /app/data
EXPOSE 8080
CMD ["node","server.js"]
